#![allow(dead_code)]

use crate::bytecode::{DebugLocation, FunctionBody};
use crate::runtime::SharedEnvironment;
use crate::value::Value;
use std::collections::{BTreeMap, VecDeque};
use std::fmt;
use std::rc::Rc;

/// Stable only within one scheduler instance. Task IDs are not artifact data
/// and must not be persisted or transferred between VM instances.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TaskId(usize);

impl TaskId {
    pub fn index(self) -> usize {
        self.0
    }
}

impl fmt::Display for TaskId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "task-{}", self.0)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TaskState {
    Ready,
    Running,
    Blocked,
    Completed,
    Failed,
    Cancelled,
}

impl TaskState {
    pub(crate) fn is_terminal(self) -> bool {
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
    TaskNotJoinable(TaskId),
    TaskAlreadyWaiting(TaskId),
    SelfJoin(TaskId),
}

impl fmt::Display for SchedulerError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidQuantum => write!(formatter, "scheduler quantum must be positive"),
            Self::TaskIdOverflow => write!(formatter, "scheduler task id exhausted"),
            Self::UnknownTask(task_id) => write!(formatter, "unknown {}", task_id),
            Self::TaskNotBlocked(task_id) => write!(formatter, "{} is not blocked", task_id),
            Self::TaskNotJoinable(task_id) => {
                write!(formatter, "{} cannot wait for a join", task_id)
            }
            Self::TaskAlreadyWaiting(task_id) => {
                write!(formatter, "{} is already waiting", task_id)
            }
            Self::SelfJoin(task_id) => write!(formatter, "{} cannot join itself", task_id),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum JoinStatus {
    Waiting,
    Ready,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ReturnTarget {
    pub(crate) register: usize,
    pub(crate) call_site: Option<DebugLocation>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FrameStackError {
    Empty,
    RootHasReturnTarget,
    MissingReturnTarget,
    InvalidReturnRegister { register: usize, available: usize },
}

impl fmt::Display for FrameStackError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Empty => write!(formatter, "frame stack is empty"),
            Self::RootHasReturnTarget => write!(formatter, "root frame has a return target"),
            Self::MissingReturnTarget => write!(formatter, "callee frame has no return target"),
            Self::InvalidReturnRegister {
                register,
                available,
            } => write!(
                formatter,
                "return register {} is outside caller register count {}",
                register, available
            ),
        }
    }
}

/// Explicit execution state for one resumable bytecode frame.
///
/// Scheduler-owned frames always carry a verified function body. The optional
/// field also lets the legacy recursive VM frame share this state container
/// without forcing ordinary single-task runs to clone the entry body.
pub(crate) struct ResumableFrame {
    pub(crate) body: Option<Rc<FunctionBody>>,
    pub(crate) ip: usize,
    pub(crate) registers: Vec<Value>,
    pub(crate) locals: SharedEnvironment,
    pub(crate) closure: SharedEnvironment,
    pub(crate) is_main: bool,
    pub(crate) function: Rc<str>,
    pub(crate) function_index: Option<usize>,
    pub(crate) return_target: Option<ReturnTarget>,
}

impl ResumableFrame {
    pub(crate) fn main(
        body: Rc<FunctionBody>,
        register_count: usize,
        locals: SharedEnvironment,
        closure: SharedEnvironment,
    ) -> Self {
        Self {
            body: Some(body),
            ip: 0,
            registers: vec![Value::Nil; register_count],
            locals,
            closure,
            is_main: true,
            function: Rc::from("main"),
            function_index: None,
            return_target: None,
        }
    }

    pub(crate) fn callee(
        body: Rc<FunctionBody>,
        function: impl Into<Rc<str>>,
        function_index: usize,
        register_count: usize,
        locals: SharedEnvironment,
        closure: SharedEnvironment,
        return_target: ReturnTarget,
    ) -> Self {
        Self {
            body: Some(body),
            ip: 0,
            registers: vec![Value::Nil; register_count],
            locals,
            closure,
            is_main: false,
            function: function.into(),
            function_index: Some(function_index),
            return_target: Some(return_target),
        }
    }
}

pub(crate) struct FrameStack {
    frames: Vec<ResumableFrame>,
}

impl FrameStack {
    pub(crate) fn new(root: ResumableFrame) -> Result<Self, FrameStackError> {
        if root.return_target.is_some() {
            return Err(FrameStackError::RootHasReturnTarget);
        }
        Ok(Self { frames: vec![root] })
    }

    pub(crate) fn len(&self) -> usize {
        self.frames.len()
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.frames.is_empty()
    }

    pub(crate) fn current(&self) -> Result<&ResumableFrame, FrameStackError> {
        self.frames.last().ok_or(FrameStackError::Empty)
    }

    pub(crate) fn current_mut(&mut self) -> Result<&mut ResumableFrame, FrameStackError> {
        self.frames.last_mut().ok_or(FrameStackError::Empty)
    }

    pub(crate) fn frames(&self) -> &[ResumableFrame] {
        &self.frames
    }

    pub(crate) fn clear(&mut self) {
        self.frames.clear();
    }

    pub(crate) fn push(&mut self, frame: ResumableFrame) -> Result<(), FrameStackError> {
        if frame.return_target.is_none() {
            return Err(FrameStackError::MissingReturnTarget);
        }
        if self.is_empty() {
            return Err(FrameStackError::Empty);
        }
        self.frames.push(frame);
        Ok(())
    }

    /// Return from the current frame. A callee result is written to its
    /// caller's pending destination; returning from the root yields the task
    /// result instead.
    pub(crate) fn return_value(&mut self, value: Value) -> Result<Option<Value>, FrameStackError> {
        let return_target = self.current()?.return_target.clone();
        let Some(return_target) = return_target else {
            let _root = self.frames.pop().ok_or(FrameStackError::Empty)?;
            return Ok(Some(value));
        };
        let caller_index = self
            .frames
            .len()
            .checked_sub(2)
            .ok_or(FrameStackError::Empty)?;
        let caller = self
            .frames
            .get(caller_index)
            .ok_or(FrameStackError::Empty)?;
        if return_target.register >= caller.registers.len() {
            return Err(FrameStackError::InvalidReturnRegister {
                register: return_target.register,
                available: caller.registers.len(),
            });
        }
        let _callee = self.frames.pop().ok_or(FrameStackError::Empty)?;
        let caller = self
            .frames
            .get_mut(caller_index)
            .ok_or(FrameStackError::Empty)?;
        caller.registers[return_target.register] = value;
        Ok(None)
    }
}

struct ScheduledTask<T> {
    payload: T,
    state: TaskState,
    cancellation_requested: bool,
}

/// Deterministic one-thread task control plane.
///
/// The VM task adapter uses `ScheduledVmTask` as the payload while preserving
/// this lifecycle and queue contract for later multi-task host APIs.
pub(crate) struct CooperativeScheduler<T> {
    quantum: usize,
    next_task_id: usize,
    tasks: BTreeMap<TaskId, ScheduledTask<T>>,
    ready: VecDeque<TaskId>,
    running: Option<TaskId>,
    join_waiters: BTreeMap<TaskId, Vec<TaskId>>,
    waiting_on: BTreeMap<TaskId, TaskId>,
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
            join_waiters: BTreeMap::new(),
            waiting_on: BTreeMap::new(),
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
        } else if next_state.is_terminal() {
            self.wake_joiners(task_id);
        }
        Ok(Some(DispatchResult {
            task_id,
            state: next_state,
        }))
    }

    pub(crate) fn wake(&mut self, task_id: TaskId) -> Result<(), SchedulerError> {
        if self.task_state(task_id)? != TaskState::Blocked {
            return Err(SchedulerError::TaskNotBlocked(task_id));
        }
        self.remove_join_waiter(task_id);
        let task = self
            .tasks
            .get_mut(&task_id)
            .ok_or(SchedulerError::UnknownTask(task_id))?;
        if task.cancellation_requested {
            task.state = TaskState::Cancelled;
            self.wake_joiners(task_id);
        } else {
            task.state = TaskState::Ready;
            self.ready.push_back(task_id);
        }
        Ok(())
    }

    /// Cancel a ready or blocked task immediately. A running task observes the
    /// request at the next checkpoint supplied by its executor callback.
    pub(crate) fn cancel(&mut self, task_id: TaskId) -> Result<(), SchedulerError> {
        let state = self.task_state(task_id)?;
        match state {
            TaskState::Ready => {
                self.ready.retain(|queued| *queued != task_id);
                self.tasks
                    .get_mut(&task_id)
                    .expect("validated task id")
                    .state = TaskState::Cancelled;
                self.wake_joiners(task_id);
            }
            TaskState::Running => {
                self.tasks
                    .get_mut(&task_id)
                    .expect("validated task id")
                    .cancellation_requested = true;
            }
            TaskState::Blocked => {
                self.remove_join_waiter(task_id);
                self.tasks
                    .get_mut(&task_id)
                    .expect("validated task id")
                    .state = TaskState::Cancelled;
                self.wake_joiners(task_id);
            }
            TaskState::Completed | TaskState::Failed | TaskState::Cancelled => {}
        }
        Ok(())
    }

    /// Register a task as waiting for another task's terminal outcome.
    ///
    /// A task that joins a non-terminal target becomes blocked. Once the
    /// target completes, fails, or is cancelled, the waiter is requeued at
    /// the tail in registration order. The value/error itself remains owned
    /// by the task payload and is consumed by the host result layer.
    pub(crate) fn join(
        &mut self,
        waiter: TaskId,
        target: TaskId,
    ) -> Result<JoinStatus, SchedulerError> {
        if waiter == target {
            return Err(SchedulerError::SelfJoin(waiter));
        }
        let waiter_state = self.task_state(waiter)?;
        let target_state = self.task_state(target)?;
        if target_state.is_terminal() {
            return Ok(JoinStatus::Ready);
        }

        match waiter_state {
            TaskState::Ready => {
                self.ready.retain(|queued| *queued != waiter);
                self.tasks
                    .get_mut(&waiter)
                    .expect("validated task id")
                    .state = TaskState::Blocked;
            }
            TaskState::Blocked => {
                if self.waiting_on.get(&waiter) == Some(&target) {
                    return Ok(JoinStatus::Waiting);
                }
                return Err(SchedulerError::TaskAlreadyWaiting(waiter));
            }
            TaskState::Running
            | TaskState::Completed
            | TaskState::Failed
            | TaskState::Cancelled => {
                return Err(SchedulerError::TaskNotJoinable(waiter));
            }
        }

        self.waiting_on.insert(waiter, target);
        self.join_waiters.entry(target).or_default().push(waiter);
        Ok(JoinStatus::Waiting)
    }

    /// Cancel every non-terminal task except the task that produced the
    /// scheduler's first failure. This is the V5A fail-fast transition.
    pub(crate) fn cancel_pending_except(&mut self, task_id: TaskId) {
        let pending = self
            .tasks
            .keys()
            .copied()
            .filter(|candidate| *candidate != task_id)
            .collect::<Vec<_>>();
        for candidate in pending {
            let _ = self.cancel(candidate);
        }
    }

    pub(crate) fn task_ids(&self) -> impl Iterator<Item = TaskId> + '_ {
        self.tasks.keys().copied()
    }

    fn remove_join_waiter(&mut self, waiter: TaskId) {
        let Some(target) = self.waiting_on.remove(&waiter) else {
            return;
        };
        if let Some(waiters) = self.join_waiters.get_mut(&target) {
            waiters.retain(|candidate| *candidate != waiter);
            if waiters.is_empty() {
                self.join_waiters.remove(&target);
            }
        }
    }

    fn wake_joiners(&mut self, target: TaskId) {
        let waiters = self.join_waiters.remove(&target).unwrap_or_default();
        for waiter in waiters {
            if self.waiting_on.get(&waiter) != Some(&target) {
                continue;
            }
            self.waiting_on.remove(&waiter);
            if self
                .tasks
                .get(&waiter)
                .is_some_and(|task| task.state == TaskState::Blocked)
            {
                self.tasks
                    .get_mut(&waiter)
                    .expect("validated waiter id")
                    .state = TaskState::Ready;
                self.ready.push_back(waiter);
            }
        }
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
    use crate::runtime::Heap;

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

    #[test]
    fn joined_waiters_wake_in_registration_order_when_target_completes() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let first_waiter = scheduler.spawn(()).expect("first waiter");
        let second_waiter = scheduler.spawn(()).expect("second waiter");
        let target = scheduler.spawn(()).expect("target");

        assert_eq!(
            scheduler.join(first_waiter, target),
            Ok(JoinStatus::Waiting)
        );
        assert_eq!(
            scheduler.join(second_waiter, target),
            Ok(JoinStatus::Waiting)
        );
        assert_eq!(scheduler.task_state(first_waiter), Ok(TaskState::Blocked));
        assert_eq!(scheduler.task_state(second_waiter), Ok(TaskState::Blocked));

        let completed = scheduler
            .dispatch(|_, _| TaskStep::Complete)
            .expect("target dispatch succeeds")
            .expect("target was ready");
        assert_eq!(completed.task_id, target);
        assert_eq!(completed.state, TaskState::Completed);
        assert_eq!(scheduler.task_state(first_waiter), Ok(TaskState::Ready));
        assert_eq!(scheduler.task_state(second_waiter), Ok(TaskState::Ready));

        let first = scheduler
            .dispatch(|_, _| TaskStep::Complete)
            .expect("first waiter dispatch succeeds")
            .expect("first waiter was woken");
        let second = scheduler
            .dispatch(|_, _| TaskStep::Complete)
            .expect("second waiter dispatch succeeds")
            .expect("second waiter was woken");
        assert_eq!(first.task_id, first_waiter);
        assert_eq!(second.task_id, second_waiter);
        assert_eq!(scheduler.join(first_waiter, target), Ok(JoinStatus::Ready));
    }

    #[test]
    fn join_rejects_self_and_duplicate_waits_and_explicit_wake_detaches_waiter() {
        let mut scheduler = CooperativeScheduler::new(1).expect("valid quantum");
        let waiter = scheduler.spawn(()).expect("waiter");
        let target = scheduler.spawn(()).expect("target");

        assert_eq!(
            scheduler.join(waiter, waiter),
            Err(SchedulerError::SelfJoin(waiter))
        );
        assert_eq!(scheduler.join(waiter, target), Ok(JoinStatus::Waiting));
        assert_eq!(scheduler.join(waiter, target), Ok(JoinStatus::Waiting));
        scheduler.wake(waiter).expect("explicit wake succeeds");
        assert_eq!(scheduler.task_state(waiter), Ok(TaskState::Ready));
        assert_eq!(scheduler.ready_len(), 2);
    }

    fn test_frame_stack() -> FrameStack {
        let heap = Heap::new();
        let locals = heap.new_environment();
        let closure = heap.new_environment();
        FrameStack::new(ResumableFrame::main(
            Rc::new(FunctionBody {
                registers: 2,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            2,
            locals,
            closure,
        ))
        .expect("valid root frame")
    }

    #[test]
    fn frame_stack_preserves_call_state_and_transfers_return_values() {
        let heap = Heap::new();
        let mut stack = test_frame_stack();
        let callee = ResumableFrame::callee(
            Rc::new(FunctionBody {
                registers: 4,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            "worker",
            3,
            4,
            heap.new_environment(),
            heap.new_environment(),
            ReturnTarget {
                register: 1,
                call_site: None,
            },
        );
        stack.push(callee).expect("callee has a caller");
        stack.current_mut().expect("callee frame").ip = 7;

        assert_eq!(stack.len(), 2);
        assert_eq!(stack.current().expect("callee frame").ip, 7);
        assert_eq!(
            stack.current().expect("callee frame").function.as_ref(),
            "worker"
        );
        assert_eq!(
            stack.current().expect("callee frame").function_index,
            Some(3)
        );
        assert!(matches!(stack.return_value(Value::number(42.0)), Ok(None)));
        assert_eq!(stack.len(), 1);
        assert!(matches!(
            stack.current().expect("root frame").registers[1],
            Value::Number(value) if value == 42.0
        ));
    }

    #[test]
    fn returning_from_root_yields_task_result_and_empties_stack() {
        let mut stack = test_frame_stack();
        assert!(matches!(
            stack.return_value(Value::string("done")),
            Ok(Some(Value::String(value))) if value == "done"
        ));
        assert!(stack.is_empty());
        assert!(matches!(stack.current(), Err(FrameStackError::Empty)));
    }

    #[test]
    fn frame_stack_rejects_invalid_root_and_return_targets() {
        let heap = Heap::new();
        let invalid_root = ResumableFrame::callee(
            Rc::new(FunctionBody {
                registers: 1,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            "root",
            0,
            1,
            heap.new_environment(),
            heap.new_environment(),
            ReturnTarget {
                register: 0,
                call_site: None,
            },
        );
        assert!(matches!(
            FrameStack::new(invalid_root),
            Err(FrameStackError::RootHasReturnTarget)
        ));

        let mut stack = test_frame_stack();
        let missing_target = ResumableFrame {
            body: Some(Rc::new(FunctionBody {
                registers: 1,
                instructions: Vec::new(),
                locations: Vec::new(),
            })),
            ip: 0,
            registers: vec![Value::Nil],
            locals: heap.new_environment(),
            closure: heap.new_environment(),
            is_main: false,
            function: Rc::from("missing"),
            function_index: Some(1),
            return_target: None,
        };
        assert_eq!(
            stack.push(missing_target),
            Err(FrameStackError::MissingReturnTarget)
        );

        let invalid_destination = ResumableFrame::callee(
            Rc::new(FunctionBody {
                registers: 1,
                instructions: Vec::new(),
                locations: Vec::new(),
            }),
            "worker",
            1,
            1,
            heap.new_environment(),
            heap.new_environment(),
            ReturnTarget {
                register: 5,
                call_site: None,
            },
        );
        stack
            .push(invalid_destination)
            .expect("callee has a caller");
        assert!(matches!(
            stack.return_value(Value::Nil),
            Err(FrameStackError::InvalidReturnRegister {
                register: 5,
                available: 2,
            })
        ));
        assert_eq!(stack.len(), 2);
    }
}
