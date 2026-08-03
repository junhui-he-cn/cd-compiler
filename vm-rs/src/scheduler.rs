#![allow(dead_code)]

use std::collections::{BTreeMap, VecDeque};
use std::fmt;

/// Stable only within one scheduler instance. Task IDs are not artifact data
/// and must not be persisted or transferred between VM instances.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct TaskId(usize);

impl TaskId {
    pub(crate) fn index(self) -> usize {
        self.0
    }
}

impl fmt::Display for TaskId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "task-{}", self.0)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TaskState {
    Ready,
    Running,
    Blocked,
    Completed,
    Failed,
    Cancelled,
}

impl TaskState {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Completed | Self::Failed | Self::Cancelled)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TaskStep {
    Yield,
    Block,
    Complete,
    Fail,
    Cancel,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct DispatchContext {
    pub(crate) task_id: TaskId,
    pub(crate) quantum: usize,
    pub(crate) cancellation_requested: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct DispatchResult {
    pub(crate) task_id: TaskId,
    pub(crate) state: TaskState,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SchedulerError {
    InvalidQuantum,
    TaskIdOverflow,
    UnknownTask(TaskId),
    TaskNotBlocked(TaskId),
}

impl fmt::Display for SchedulerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidQuantum => write!(formatter, "scheduler quantum must be positive"),
            Self::TaskIdOverflow => write!(formatter, "scheduler task id exhausted"),
            Self::UnknownTask(task_id) => write!(formatter, "unknown {}", task_id),
            Self::TaskNotBlocked(task_id) => write!(formatter, "{} is not blocked", task_id),
        }
    }
}

struct ScheduledTask<T> {
    payload: T,
    state: TaskState,
    cancellation_requested: bool,
}

/// Deterministic one-thread task control plane.
///
/// This first V5B slice deliberately schedules opaque payloads. The next
/// slice will use an explicit bytecode frame stack as the payload while
/// preserving this lifecycle and queue contract.
pub(crate) struct CooperativeScheduler<T> {
    quantum: usize,
    next_task_id: usize,
    tasks: BTreeMap<TaskId, ScheduledTask<T>>,
    ready: VecDeque<TaskId>,
    running: Option<TaskId>,
}

impl<T> CooperativeScheduler<T> {
    pub(crate) fn new(quantum: usize) -> Result<Self, SchedulerError> {
        if quantum == 0 {
            return Err(SchedulerError::InvalidQuantum);
        }
        Ok(Self {
            quantum,
            next_task_id: 0,
            tasks: BTreeMap::new(),
            ready: VecDeque::new(),
            running: None,
        })
    }

    pub(crate) fn quantum(&self) -> usize {
        self.quantum
    }

    pub(crate) fn spawn(&mut self, payload: T) -> Result<TaskId, SchedulerError> {
        let task_id = TaskId(self.next_task_id);
        self.next_task_id = self
            .next_task_id
            .checked_add(1)
            .ok_or(SchedulerError::TaskIdOverflow)?;
        self.tasks.insert(
            task_id,
            ScheduledTask {
                payload,
                state: TaskState::Ready,
                cancellation_requested: false,
            },
        );
        self.ready.push_back(task_id);
        Ok(task_id)
    }

    pub(crate) fn task_state(&self, task_id: TaskId) -> Result<TaskState, SchedulerError> {
        self.tasks
            .get(&task_id)
            .map(|task| task.state)
            .ok_or(SchedulerError::UnknownTask(task_id))
    }

    pub(crate) fn task_payload(&self, task_id: TaskId) -> Result<&T, SchedulerError> {
        self.tasks
            .get(&task_id)
            .map(|task| &task.payload)
            .ok_or(SchedulerError::UnknownTask(task_id))
    }

    pub(crate) fn task_payload_mut(&mut self, task_id: TaskId) -> Result<&mut T, SchedulerError> {
        self.tasks
            .get_mut(&task_id)
            .map(|task| &mut task.payload)
            .ok_or(SchedulerError::UnknownTask(task_id))
    }

    pub(crate) fn running_task(&self) -> Option<TaskId> {
        self.running
    }

    pub(crate) fn ready_len(&self) -> usize {
        self.ready.len()
    }

    pub(crate) fn blocked_len(&self) -> usize {
        self.tasks
            .values()
            .filter(|task| task.state == TaskState::Blocked)
            .count()
    }

    pub(crate) fn is_complete(&self) -> bool {
        self.tasks.values().all(|task| task.state.is_terminal())
    }

    pub(crate) fn is_waiting(&self) -> bool {
        self.running.is_none() && self.ready.is_empty() && !self.is_complete()
    }

    /// Dispatch one ready task. `None` means the scheduler is currently
    /// waiting for a wake event or all tasks are terminal.
    pub(crate) fn dispatch<F>(
        &mut self,
        execute: F,
    ) -> Result<Option<DispatchResult>, SchedulerError>
    where
        F: FnOnce(&mut T, DispatchContext) -> TaskStep,
    {
        let Some(task_id) = self.pop_ready_task() else {
            return Ok(None);
        };
        let task = self
            .tasks
            .get_mut(&task_id)
            .ok_or(SchedulerError::UnknownTask(task_id))?;
        task.state = TaskState::Running;
        self.running = Some(task_id);
        let context = DispatchContext {
            task_id,
            quantum: self.quantum,
            cancellation_requested: task.cancellation_requested,
        };
        let step = execute(&mut task.payload, context);
        self.running = None;

        let task = self
            .tasks
            .get_mut(&task_id)
            .ok_or(SchedulerError::UnknownTask(task_id))?;
        let next_state = if task.cancellation_requested {
            TaskState::Cancelled
        } else {
            match step {
                TaskStep::Yield => TaskState::Ready,
                TaskStep::Block => TaskState::Blocked,
                TaskStep::Complete => TaskState::Completed,
                TaskStep::Fail => TaskState::Failed,
                TaskStep::Cancel => TaskState::Cancelled,
            }
        };
        task.state = next_state;
        if next_state == TaskState::Ready {
            self.ready.push_back(task_id);
        }
        Ok(Some(DispatchResult {
            task_id,
            state: next_state,
        }))
    }

    pub(crate) fn wake(&mut self, task_id: TaskId) -> Result<(), SchedulerError> {
        let task = self
            .tasks
            .get_mut(&task_id)
            .ok_or(SchedulerError::UnknownTask(task_id))?;
        if task.state != TaskState::Blocked {
            return Err(SchedulerError::TaskNotBlocked(task_id));
        }
        if task.cancellation_requested {
            task.state = TaskState::Cancelled;
        } else {
            task.state = TaskState::Ready;
            self.ready.push_back(task_id);
        }
        Ok(())
    }

    /// Cancel a ready or blocked task immediately. A running task observes the
    /// request at the next checkpoint supplied by its executor callback.
    pub(crate) fn cancel(&mut self, task_id: TaskId) -> Result<(), SchedulerError> {
        let task = self
            .tasks
            .get_mut(&task_id)
            .ok_or(SchedulerError::UnknownTask(task_id))?;
        match task.state {
            TaskState::Ready => {
                task.state = TaskState::Cancelled;
                self.ready.retain(|queued| *queued != task_id);
            }
            TaskState::Running => task.cancellation_requested = true,
            TaskState::Blocked => task.state = TaskState::Cancelled,
            TaskState::Completed | TaskState::Failed | TaskState::Cancelled => {}
        }
        Ok(())
    }

    fn pop_ready_task(&mut self) -> Option<TaskId> {
        while let Some(task_id) = self.ready.pop_front() {
            if self
                .tasks
                .get(&task_id)
                .is_some_and(|task| task.state == TaskState::Ready)
            {
                return Some(task_id);
            }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_zero_quantum() {
        assert!(matches!(
            CooperativeScheduler::<()>::new(0),
            Err(SchedulerError::InvalidQuantum)
        ));
    }

    #[test]
    fn fifo_yield_requeues_tasks_at_the_tail() {
        let mut scheduler = CooperativeScheduler::new(3).expect("valid quantum");
        let first = scheduler.spawn(0usize).expect("first task");
        let second = scheduler.spawn(0usize).expect("second task");
        let mut order = Vec::new();

        for _ in 0..4 {
            scheduler
                .dispatch(|value, context| {
                    assert_eq!(context.quantum, 3);
                    order.push(context.task_id);
                    *value += 1;
                    if *value == 2 {
                        TaskStep::Complete
                    } else {
                        TaskStep::Yield
                    }
                })
                .expect("dispatch succeeds");
        }

        assert_eq!(order, vec![first, second, first, second]);
        assert_eq!(scheduler.task_state(first), Ok(TaskState::Completed));
        assert_eq!(scheduler.task_state(second), Ok(TaskState::Completed));
        assert!(scheduler.is_complete());
        assert_eq!(scheduler.ready_len(), 0);
    }

    #[test]
    fn blocked_tasks_wake_in_explicit_order() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let blocked = scheduler.spawn(false).expect("blocked task");
        let ready = scheduler.spawn(true).expect("ready task");

        let first = scheduler
            .dispatch(|value, _| {
                assert!(!*value);
                TaskStep::Block
            })
            .expect("dispatch succeeds")
            .expect("task was ready");
        assert_eq!(first.task_id, blocked);
        assert_eq!(first.state, TaskState::Blocked);
        assert!(scheduler.is_waiting() == false);

        let second = scheduler
            .dispatch(|value, _| {
                assert!(*value);
                TaskStep::Complete
            })
            .expect("dispatch succeeds")
            .expect("task was ready");
        assert_eq!(second.task_id, ready);
        assert_eq!(second.state, TaskState::Completed);
        assert!(scheduler.is_waiting());

        scheduler.wake(blocked).expect("blocked task wakes");
        assert_eq!(scheduler.ready_len(), 1);
        scheduler
            .dispatch(|value, _| {
                *value = true;
                TaskStep::Complete
            })
            .expect("dispatch succeeds");
        assert_eq!(scheduler.task_state(blocked), Ok(TaskState::Completed));
        assert!(scheduler.is_complete());
    }

    #[test]
    fn cancellation_removes_ready_tasks_and_marks_blocked_tasks() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let ready = scheduler.spawn(()).expect("ready task");
        let blocked = scheduler.spawn(()).expect("blocked task");

        scheduler.cancel(ready).expect("ready task cancels");
        assert_eq!(scheduler.task_state(ready), Ok(TaskState::Cancelled));
        let result = scheduler
            .dispatch(|_, _| TaskStep::Block)
            .expect("dispatch succeeds")
            .expect("blocked task was ready");
        assert_eq!(result.task_id, blocked);
        scheduler.cancel(blocked).expect("blocked task cancels");
        assert_eq!(scheduler.task_state(blocked), Ok(TaskState::Cancelled));
        assert!(scheduler.is_complete());
    }

    #[test]
    fn cancelling_a_ready_task_removes_it_from_the_queue() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let task = scheduler.spawn(()).expect("task");
        scheduler.cancel(task).expect("ready task cancels");
        assert_eq!(scheduler.task_state(task), Ok(TaskState::Cancelled));
        assert!(scheduler.dispatch(|_, _| TaskStep::Fail).unwrap().is_none());
    }

    #[test]
    fn unknown_and_non_blocked_wake_are_reported() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let task = scheduler.spawn(()).expect("task");
        let unknown = TaskId(99);
        assert_eq!(
            scheduler.task_state(unknown),
            Err(SchedulerError::UnknownTask(unknown))
        );
        assert_eq!(
            scheduler.wake(task),
            Err(SchedulerError::TaskNotBlocked(task))
        );
    }
}
