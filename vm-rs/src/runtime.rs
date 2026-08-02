#![allow(dead_code)]

use crate::value::Value;
use std::cell::{Cell as ScalarCell, RefCell};
use std::collections::{HashMap, HashSet, VecDeque};
use std::fmt;
use std::mem::size_of;
use std::ops::Deref;
use std::rc::Rc;
use std::rc::Weak;

pub type Cell = Rc<TrackedStorage<Value>>;
pub type Environment = HashMap<String, Cell>;
pub type SharedEnvironment = Rc<TrackedStorage<Environment>>;
pub type SharedArrayElements = Rc<TrackedStorage<Vec<Value>>>;
pub type SharedMapEntries = Rc<TrackedStorage<Vec<(Value, Value)>>>;
pub type SharedStructFields = Rc<TrackedStorage<Vec<(String, Value)>>>;

#[derive(Clone, Debug)]
pub struct FunctionValue {
    pub name: String,
    pub function_index: usize,
    pub arity: usize,
    pub identity: usize,
    pub closure: SharedEnvironment,
}

#[derive(Clone, Debug)]
pub struct ArrayValue {
    pub identity: usize,
    pub elements: SharedArrayElements,
}

#[derive(Clone, Debug)]
pub struct MapValue {
    pub identity: usize,
    pub entries: SharedMapEntries,
}

#[derive(Clone, Debug)]
pub struct RangeValue {
    pub start: i64,
    pub stop: i64,
    pub step: i64,
    pub length: usize,
}

#[derive(Clone, Debug)]
pub struct StructValue {
    pub identity: usize,
    pub type_name: Option<String>,
    pub fields: SharedStructFields,
}

#[derive(Clone, Debug)]
pub struct VariantValue {
    pub enum_name: String,
    pub variant_name: String,
    pub fields: Vec<Value>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum HeapObjectKind {
    Environment,
    Cell,
    Array,
    Map,
    Struct,
}

impl HeapObjectKind {
    const COUNT: usize = 5;

    const fn index(self) -> usize {
        match self {
            Self::Environment => 0,
            Self::Cell => 1,
            Self::Array => 2,
            Self::Map => 3,
            Self::Struct => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct HeapObjectStats {
    pub allocations: usize,
    pub live: usize,
    pub dead: usize,
    /// Estimated bytes retained by currently live storage of this kind.
    pub estimated_bytes: usize,
    /// Maximum estimated bytes retained by this kind during observation.
    pub peak_estimated_bytes: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HeapStatsSnapshot {
    by_kind: [HeapObjectStats; HeapObjectKind::COUNT],
    pub total_allocations: usize,
    pub total_live: usize,
    pub total_dead: usize,
    pub peak_live: usize,
    /// Estimated bytes retained by all currently live tracked storage.
    pub estimated_live_bytes: usize,
    /// Maximum estimated bytes retained by all tracked storage during observation.
    pub estimated_peak_live_bytes: usize,
}

impl HeapStatsSnapshot {
    pub fn for_kind(&self, kind: HeapObjectKind) -> HeapObjectStats {
        self.by_kind[kind.index()]
    }
}

#[derive(Clone, Debug)]
pub struct HeapStats {
    ledger: Rc<RefCell<HeapLedger>>,
}

impl HeapStats {
    pub fn snapshot(&self) -> HeapStatsSnapshot {
        let mut ledger = self.ledger.borrow_mut();
        ledger.observe_estimated_bytes();
        ledger.snapshot()
    }
}

#[derive(Clone, Debug)]
enum WeakAllocation {
    Environment(Weak<TrackedStorage<Environment>>),
    Cell(Weak<TrackedStorage<Value>>),
    Array(Weak<TrackedStorage<Vec<Value>>>),
    Map(Weak<TrackedStorage<Vec<(Value, Value)>>>),
    Struct(Weak<TrackedStorage<Vec<(String, Value)>>>),
}

impl WeakAllocation {
    fn kind(&self) -> HeapObjectKind {
        match self {
            Self::Environment(_) => HeapObjectKind::Environment,
            Self::Cell(_) => HeapObjectKind::Cell,
            Self::Array(_) => HeapObjectKind::Array,
            Self::Map(_) => HeapObjectKind::Map,
            Self::Struct(_) => HeapObjectKind::Struct,
        }
    }

    fn observe_estimated_bytes(&self) -> Option<usize> {
        match self {
            Self::Environment(value) => value
                .upgrade()
                .map(|storage| storage.observe_estimated_bytes()),
            Self::Cell(value) => value
                .upgrade()
                .map(|storage| storage.observe_estimated_bytes()),
            Self::Array(value) => value
                .upgrade()
                .map(|storage| storage.observe_estimated_bytes()),
            Self::Map(value) => value
                .upgrade()
                .map(|storage| storage.observe_estimated_bytes()),
            Self::Struct(value) => value
                .upgrade()
                .map(|storage| storage.observe_estimated_bytes()),
        }
    }

    fn inspect(&self) -> Option<AllocationInfo> {
        match self {
            Self::Environment(value) => value.upgrade().and_then(|storage| {
                let pointer = Rc::as_ptr(&storage) as usize;
                let strong_count = Rc::strong_count(&storage).saturating_sub(1);
                let outgoing = storage.try_borrow().ok().map(|environment| {
                    environment
                        .values()
                        .map(|cell| Rc::as_ptr(cell) as usize)
                        .collect()
                })?;
                Some(AllocationInfo {
                    allocation: self.clone(),
                    pointer,
                    strong_count,
                    outgoing,
                })
            }),
            Self::Cell(value) => value.upgrade().and_then(|storage| {
                let pointer = Rc::as_ptr(&storage) as usize;
                let strong_count = Rc::strong_count(&storage).saturating_sub(1);
                let outgoing = storage.try_borrow().ok().map(|value| {
                    let mut outgoing = Vec::new();
                    collect_value_references(&value, &mut outgoing);
                    outgoing
                })?;
                Some(AllocationInfo {
                    allocation: self.clone(),
                    pointer,
                    strong_count,
                    outgoing,
                })
            }),
            Self::Array(value) => value.upgrade().and_then(|storage| {
                let pointer = Rc::as_ptr(&storage) as usize;
                let strong_count = Rc::strong_count(&storage).saturating_sub(1);
                let outgoing = storage.try_borrow().ok().map(|values| {
                    let mut outgoing = Vec::new();
                    for value in values.iter() {
                        collect_value_references(value, &mut outgoing);
                    }
                    outgoing
                })?;
                Some(AllocationInfo {
                    allocation: self.clone(),
                    pointer,
                    strong_count,
                    outgoing,
                })
            }),
            Self::Map(value) => value.upgrade().and_then(|storage| {
                let pointer = Rc::as_ptr(&storage) as usize;
                let strong_count = Rc::strong_count(&storage).saturating_sub(1);
                let outgoing = storage.try_borrow().ok().map(|entries| {
                    let mut outgoing = Vec::new();
                    for (key, value) in entries.iter() {
                        collect_value_references(key, &mut outgoing);
                        collect_value_references(value, &mut outgoing);
                    }
                    outgoing
                })?;
                Some(AllocationInfo {
                    allocation: self.clone(),
                    pointer,
                    strong_count,
                    outgoing,
                })
            }),
            Self::Struct(value) => value.upgrade().and_then(|storage| {
                let pointer = Rc::as_ptr(&storage) as usize;
                let strong_count = Rc::strong_count(&storage).saturating_sub(1);
                let outgoing = storage.try_borrow().ok().map(|fields| {
                    let mut outgoing = Vec::new();
                    for (_, value) in fields.iter() {
                        collect_value_references(value, &mut outgoing);
                    }
                    outgoing
                })?;
                Some(AllocationInfo {
                    allocation: self.clone(),
                    pointer,
                    strong_count,
                    outgoing,
                })
            }),
        }
    }

    fn clear_references(&self) -> bool {
        match self {
            Self::Environment(value) => value.upgrade().is_some_and(|storage| {
                let Ok(mut environment) = storage.try_borrow_mut() else {
                    return false;
                };
                environment.clear();
                true
            }),
            Self::Cell(value) => value.upgrade().is_some_and(|storage| {
                let Ok(mut value) = storage.try_borrow_mut() else {
                    return false;
                };
                *value = Value::Nil;
                true
            }),
            Self::Array(value) => value.upgrade().is_some_and(|storage| {
                let Ok(mut values) = storage.try_borrow_mut() else {
                    return false;
                };
                values.clear();
                true
            }),
            Self::Map(value) => value.upgrade().is_some_and(|storage| {
                let Ok(mut entries) = storage.try_borrow_mut() else {
                    return false;
                };
                entries.clear();
                true
            }),
            Self::Struct(value) => value.upgrade().is_some_and(|storage| {
                let Ok(mut fields) = storage.try_borrow_mut() else {
                    return false;
                };
                fields.clear();
                true
            }),
        }
    }
}

#[derive(Debug)]
struct AllocationInfo {
    allocation: WeakAllocation,
    pointer: usize,
    strong_count: usize,
    outgoing: Vec<usize>,
}

fn collect_value_references(value: &Value, outgoing: &mut Vec<usize>) {
    match value {
        Value::Function(value) => outgoing.push(Rc::as_ptr(&value.closure) as usize),
        Value::Array(value) => outgoing.push(Rc::as_ptr(&value.elements) as usize),
        Value::Map(value) => outgoing.push(Rc::as_ptr(&value.entries) as usize),
        Value::Struct(value) => outgoing.push(Rc::as_ptr(&value.fields) as usize),
        Value::Variant(value) => {
            for field in &value.fields {
                collect_value_references(field, outgoing);
            }
        }
        Value::Nil | Value::Number(_) | Value::Bool(_) | Value::String(_) | Value::Range(_) => {}
    }
}

#[derive(Debug, Default)]
struct HeapLedger {
    allocations: Vec<WeakAllocation>,
    live: [usize; HeapObjectKind::COUNT],
    peak_live: usize,
    track_estimated_bytes: bool,
    live_estimated_bytes: [usize; HeapObjectKind::COUNT],
    peak_estimated_bytes: [usize; HeapObjectKind::COUNT],
    peak_estimated_live_bytes: usize,
}

impl HeapLedger {
    fn record(&mut self, allocation: WeakAllocation) {
        let kind = allocation.kind();
        self.allocations.push(allocation);
        self.live[kind.index()] += 1;
        self.peak_live = self.live.iter().sum::<usize>().max(self.peak_live);
        self.observe_estimated_bytes();
    }

    fn release(&mut self, kind: HeapObjectKind, estimated_bytes: usize) {
        let live = &mut self.live[kind.index()];
        debug_assert!(*live > 0, "heap live counter underflow for {:?}", kind);
        if *live > 0 {
            *live -= 1;
        }
        if self.track_estimated_bytes {
            let bytes = &mut self.live_estimated_bytes[kind.index()];
            *bytes = bytes.saturating_sub(estimated_bytes);
        }
    }

    fn observe_estimated_bytes(&mut self) {
        if !self.track_estimated_bytes {
            return;
        }
        let mut live_estimated_bytes = [0usize; HeapObjectKind::COUNT];
        for allocation in &self.allocations {
            if let Some(estimated_bytes) = allocation.observe_estimated_bytes() {
                let bytes = &mut live_estimated_bytes[allocation.kind().index()];
                *bytes = bytes.saturating_add(estimated_bytes);
            }
        }
        self.live_estimated_bytes = live_estimated_bytes;
        self.peak_estimated_live_bytes = self
            .peak_estimated_live_bytes
            .max(live_estimated_bytes.iter().sum());
        for (index, estimated_bytes) in live_estimated_bytes.iter().enumerate() {
            self.peak_estimated_bytes[index] =
                self.peak_estimated_bytes[index].max(*estimated_bytes);
        }
    }

    fn snapshot(&self) -> HeapStatsSnapshot {
        let mut by_kind = [HeapObjectStats::default(); HeapObjectKind::COUNT];
        for allocation in &self.allocations {
            let kind = allocation.kind();
            let stats = &mut by_kind[kind.index()];
            stats.allocations += 1;
        }
        for (index, stats) in by_kind.iter_mut().enumerate() {
            stats.live = self.live[index];
            stats.estimated_bytes = self.live_estimated_bytes[index];
            stats.peak_estimated_bytes = self.peak_estimated_bytes[index];
        }
        for stats in &mut by_kind {
            stats.dead = stats.allocations - stats.live;
        }
        let total_allocations = by_kind.iter().map(|stats| stats.allocations).sum();
        let total_live = by_kind.iter().map(|stats| stats.live).sum();
        HeapStatsSnapshot {
            by_kind,
            total_allocations,
            total_live,
            total_dead: total_allocations - total_live,
            peak_live: self.peak_live,
            estimated_live_bytes: self.live_estimated_bytes.iter().sum(),
            estimated_peak_live_bytes: self.peak_estimated_live_bytes,
        }
    }

    fn profile_counts(&self) -> (usize, usize) {
        (self.allocations.len(), self.peak_live)
    }

    fn unreachable_allocations(&self) -> Vec<WeakAllocation> {
        let infos: Vec<AllocationInfo> = self
            .allocations
            .iter()
            .filter_map(WeakAllocation::inspect)
            .collect();
        let mut by_pointer = HashMap::with_capacity(infos.len());
        let mut incoming = HashMap::with_capacity(infos.len());
        for (index, info) in infos.iter().enumerate() {
            by_pointer.insert(info.pointer, index);
            incoming.insert(info.pointer, 0usize);
        }
        for info in &infos {
            for pointer in &info.outgoing {
                if let Some(count) = incoming.get_mut(pointer) {
                    *count = count.saturating_add(1);
                }
            }
        }

        let mut marked = HashSet::with_capacity(infos.len());
        let mut worklist = VecDeque::new();
        for info in &infos {
            if info.strong_count > incoming.get(&info.pointer).copied().unwrap_or(0) {
                marked.insert(info.pointer);
                worklist.push_back(info.pointer);
            }
        }
        while let Some(pointer) = worklist.pop_front() {
            let Some(index) = by_pointer.get(&pointer).copied() else {
                continue;
            };
            for outgoing in &infos[index].outgoing {
                if by_pointer.contains_key(outgoing) && marked.insert(*outgoing) {
                    worklist.push_back(*outgoing);
                }
            }
        }

        infos
            .into_iter()
            .filter(|info| !marked.contains(&info.pointer))
            .map(|info| info.allocation)
            .collect()
    }
}

/// Shared mutable storage with a non-owning accounting token.
///
/// The `Rc` remains the alias/lifetime boundary. The token only holds a weak
/// ledger reference, so accounting cannot keep storage alive or change cycle
/// behavior.
#[derive(Debug)]
pub struct TrackedStorage<T> {
    value: RefCell<T>,
    ledger: Weak<RefCell<HeapLedger>>,
    kind: HeapObjectKind,
    size_estimator: fn(&T) -> usize,
    accounted_bytes: ScalarCell<usize>,
}

impl<T> Deref for TrackedStorage<T> {
    type Target = RefCell<T>;

    fn deref(&self) -> &Self::Target {
        &self.value
    }
}

impl<T> TrackedStorage<T> {
    fn observe_estimated_bytes(&self) -> usize {
        let estimated_bytes = self
            .value
            .try_borrow()
            .map(|value| {
                size_of::<Self>()
                    .saturating_add(2usize.saturating_mul(size_of::<usize>()))
                    .saturating_add((self.size_estimator)(&value))
            })
            .unwrap_or_else(|_| self.accounted_bytes.get());
        self.accounted_bytes.set(estimated_bytes);
        estimated_bytes
    }
}

impl<T> Drop for TrackedStorage<T> {
    fn drop(&mut self) {
        if let Some(ledger) = self.ledger.upgrade() {
            ledger
                .borrow_mut()
                .release(self.kind, self.accounted_bytes.get());
        }
    }
}

fn estimate_inline_value_dynamic_bytes(value: &Value) -> usize {
    let mut bytes = 0usize;
    match value {
        Value::String(value) => {
            bytes = bytes.saturating_add(value.capacity());
        }
        Value::Function(value) => {
            bytes = bytes.saturating_add(value.name.capacity());
        }
        Value::Variant(value) => {
            bytes = bytes
                .saturating_add(value.enum_name.capacity())
                .saturating_add(value.variant_name.capacity())
                .saturating_add(value.fields.capacity().saturating_mul(size_of::<Value>()));
            for field in &value.fields {
                bytes = bytes.saturating_add(estimate_inline_value_dynamic_bytes(field));
            }
        }
        _ => {}
    }
    bytes
}

fn estimate_environment_payload(value: &Environment) -> usize {
    let mut bytes = value.capacity().saturating_mul(size_of::<(String, Cell)>());
    for (name, _) in value {
        bytes = bytes.saturating_add(name.capacity());
    }
    bytes
}

fn estimate_cell_payload(value: &Value) -> usize {
    estimate_inline_value_dynamic_bytes(value)
}

fn estimate_array_payload(value: &Vec<Value>) -> usize {
    let mut bytes = value.capacity().saturating_mul(size_of::<Value>());
    for element in value {
        bytes = bytes.saturating_add(estimate_inline_value_dynamic_bytes(element));
    }
    bytes
}

fn estimate_map_payload(value: &Vec<(Value, Value)>) -> usize {
    let mut bytes = value.capacity().saturating_mul(size_of::<(Value, Value)>());
    for (key, value) in value {
        bytes = bytes
            .saturating_add(estimate_inline_value_dynamic_bytes(key))
            .saturating_add(estimate_inline_value_dynamic_bytes(value));
    }
    bytes
}

fn estimate_struct_payload(value: &Vec<(String, Value)>) -> usize {
    let mut bytes = value
        .capacity()
        .saturating_mul(size_of::<(String, Value)>());
    for (name, value) in value {
        bytes = bytes
            .saturating_add(name.capacity())
            .saturating_add(estimate_inline_value_dynamic_bytes(value));
    }
    bytes
}

fn tracked_storage<T>(
    ledger: &Rc<RefCell<HeapLedger>>,
    kind: HeapObjectKind,
    value: T,
    size_estimator: fn(&T) -> usize,
    make_weak: impl FnOnce(Weak<TrackedStorage<T>>) -> WeakAllocation,
) -> Rc<TrackedStorage<T>> {
    let storage = Rc::new(TrackedStorage {
        value: RefCell::new(value),
        ledger: Rc::downgrade(ledger),
        kind,
        size_estimator,
        accounted_bytes: ScalarCell::new(0),
    });
    ledger
        .borrow_mut()
        .record(make_weak(Rc::downgrade(&storage)));
    storage
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HeapError;

impl fmt::Display for HeapError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("runtime identity exhausted")
    }
}

/// Non-moving tracing heap over stable, identity-bearing storage.
///
/// The storage address remains stable for the lifetime of an object and the
/// existing `Rc<RefCell<...>>` representation remains the compatibility layer
/// for aliases and mutation. At collection safepoints the ledger derives
/// external roots from strong-reference counts, traces tracked edges, and
/// clears unreachable storage so reference-counted cycles can be reclaimed.
/// The collector never moves objects or changes value identity.
#[derive(Debug)]
pub struct Heap {
    next_function_identity: usize,
    next_array_identity: usize,
    next_map_identity: usize,
    next_struct_identity: usize,
    track_estimated_bytes: ScalarCell<bool>,
    ledger: Rc<RefCell<HeapLedger>>,
}

impl Default for Heap {
    fn default() -> Self {
        Self::new()
    }
}

impl Heap {
    pub fn new() -> Self {
        Self {
            next_function_identity: 1,
            next_array_identity: 1,
            next_map_identity: 1,
            next_struct_identity: 1,
            track_estimated_bytes: ScalarCell::new(false),
            ledger: Rc::new(RefCell::new(HeapLedger::default())),
        }
    }

    pub fn stats(&self) -> HeapStats {
        self.track_estimated_bytes.set(true);
        let mut ledger = self.ledger.borrow_mut();
        ledger.track_estimated_bytes = true;
        ledger.observe_estimated_bytes();
        drop(ledger);
        HeapStats {
            ledger: self.ledger.clone(),
        }
    }

    pub(crate) fn observe_estimated_bytes(&self) {
        if !self.track_estimated_bytes.get() {
            return;
        }
        self.ledger.borrow_mut().observe_estimated_bytes();
    }

    pub(crate) fn profile_counts(&self) -> (usize, usize) {
        self.ledger.borrow().profile_counts()
    }

    /// Collect unreachable tracked storage without moving live objects.
    ///
    /// All strong references outside tracked storage are treated as roots. A
    /// caller must invoke this at a VM safepoint, after transient borrows and
    /// temporary values have ended. The return value is the number of tracked
    /// storage objects whose references were cleared.
    pub fn collect_garbage(&self) -> usize {
        let unreachable = self.ledger.borrow().unreachable_allocations();
        let mut collected = 0;
        for allocation in unreachable {
            if allocation.clear_references() {
                collected += 1;
            }
        }
        self.observe_estimated_bytes();
        collected
    }

    pub fn new_environment(&self) -> SharedEnvironment {
        tracked_storage(
            &self.ledger,
            HeapObjectKind::Environment,
            HashMap::new(),
            estimate_environment_payload,
            WeakAllocation::Environment,
        )
    }

    pub fn new_cell(&self, value: Value) -> Cell {
        tracked_storage(
            &self.ledger,
            HeapObjectKind::Cell,
            value,
            estimate_cell_payload,
            WeakAllocation::Cell,
        )
    }

    fn next_identity(counter: &mut usize) -> Result<usize, HeapError> {
        let identity = *counter;
        *counter = counter.checked_add(1).ok_or(HeapError)?;
        Ok(identity)
    }

    pub fn allocate_function(
        &mut self,
        name: impl Into<String>,
        function_index: usize,
        arity: usize,
        closure: SharedEnvironment,
    ) -> Result<Value, HeapError> {
        let identity = Self::next_identity(&mut self.next_function_identity)?;
        Ok(Value::function(FunctionValue {
            name: name.into(),
            function_index,
            arity,
            identity,
            closure,
        }))
    }

    pub fn allocate_array(&mut self, elements: Vec<Value>) -> Result<Value, HeapError> {
        let identity = Self::next_identity(&mut self.next_array_identity)?;
        let elements = tracked_storage(
            &self.ledger,
            HeapObjectKind::Array,
            elements,
            estimate_array_payload,
            WeakAllocation::Array,
        );
        Ok(Value::array(ArrayValue { identity, elements }))
    }

    fn normalize_map_entries(entries: Vec<(Value, Value)>) -> Vec<(Value, Value)> {
        let mut ordered: Vec<(Value, Value)> = Vec::with_capacity(entries.len());
        for (key, value) in entries {
            if let Some((_, existing)) = ordered
                .iter_mut()
                .find(|(existing_key, _)| existing_key.runtime_equals(&key))
            {
                *existing = value;
            } else {
                ordered.push((key, value));
            }
        }
        ordered
    }

    pub fn map_entry_count(entries: &[(Value, Value)]) -> usize {
        Self::normalize_map_entries(entries.to_vec()).len()
    }

    pub fn allocate_map(&mut self, entries: Vec<(Value, Value)>) -> Result<Value, HeapError> {
        let ordered = Self::normalize_map_entries(entries);
        let identity = Self::next_identity(&mut self.next_map_identity)?;
        let entries = tracked_storage(
            &self.ledger,
            HeapObjectKind::Map,
            ordered,
            estimate_map_payload,
            WeakAllocation::Map,
        );
        Ok(Value::map(MapValue { identity, entries }))
    }

    pub fn allocate_range(&self, start: i64, stop: i64, step: i64, length: usize) -> Value {
        Value::range(RangeValue {
            start,
            stop,
            step,
            length,
        })
    }

    pub fn allocate_struct(
        &mut self,
        type_name: Option<String>,
        fields: Vec<(String, Value)>,
    ) -> Result<Value, HeapError> {
        let identity = Self::next_identity(&mut self.next_struct_identity)?;
        let fields = tracked_storage(
            &self.ledger,
            HeapObjectKind::Struct,
            fields,
            estimate_struct_payload,
            WeakAllocation::Struct,
        );
        Ok(Value::structure(StructValue {
            identity,
            type_name,
            fields,
        }))
    }

    pub fn allocate_variant(
        &self,
        enum_name: String,
        variant_name: String,
        fields: Vec<Value>,
    ) -> Value {
        Value::variant(VariantValue {
            enum_name,
            variant_name,
            fields,
        })
    }
}

pub fn new_environment() -> SharedEnvironment {
    Heap::new().new_environment()
}

pub fn new_cell(value: Value) -> Cell {
    Heap::new().new_cell(value)
}

#[cfg(test)]
mod tests {
    use super::{Heap, HeapObjectKind};
    use crate::value::Value;

    #[test]
    fn heap_allocates_distinct_identity_handles_with_shared_alias_storage() {
        let mut heap = Heap::new();
        let first = heap
            .allocate_array(vec![Value::number(1.0)])
            .expect("first array identity should be available");
        let alias = first.clone();
        let second = heap
            .allocate_array(vec![Value::number(1.0)])
            .expect("second array identity should be available");

        assert!(first.runtime_equals(&alias));
        assert!(!first.runtime_equals(&second));
        if let Value::Array(array) = alias {
            array.elements.borrow_mut().push(Value::number(2.0));
        } else {
            panic!("expected array alias");
        }
        assert_eq!(first.to_string(), "[1, 2]");
        assert_eq!(second.to_string(), "[1]");
    }

    #[test]
    fn heap_environment_and_cells_share_mutable_binding_state() {
        let heap = Heap::new();
        let environment = heap.new_environment();
        let cell = heap.new_cell(Value::number(1.0));
        environment
            .borrow_mut()
            .insert("value".to_string(), cell.clone());

        *cell.borrow_mut() = Value::number(2.0);
        assert_eq!(environment.borrow()["value"].borrow().to_string(), "2");
    }

    #[test]
    fn heap_map_factory_preserves_first_key_position_and_replaces_duplicates() {
        let mut heap = Heap::new();
        let map = heap
            .allocate_map(vec![
                (Value::string("a"), Value::number(1.0)),
                (Value::string("b"), Value::number(2.0)),
                (Value::string("a"), Value::number(3.0)),
            ])
            .expect("map identity should be available");

        assert_eq!(map.to_string(), "map{a: 3, b: 2}");
        assert_eq!(
            Heap::map_entry_count(&[(Value::string("a"), Value::Nil)]),
            1
        );
    }

    #[test]
    fn heap_stats_reports_shared_storage_live_and_dead_without_retaining_values() {
        let mut heap = Heap::new();
        let stats = heap.stats();
        let environment = heap.new_environment();
        let cell = heap.new_cell(Value::number(1.0));
        let array = heap
            .allocate_array(vec![Value::number(1.0)])
            .expect("array identity should be available");
        let map = heap
            .allocate_map(vec![(Value::string("key"), Value::number(1.0))])
            .expect("map identity should be available");
        let structure = heap
            .allocate_struct(None, vec![("value".to_string(), Value::number(1.0))])
            .expect("struct identity should be available");

        let live = stats.snapshot();
        assert_eq!(live.total_allocations, 5);
        assert_eq!(live.total_live, 5);
        assert_eq!(live.total_dead, 0);
        assert_eq!(live.peak_live, 5);
        for kind in [
            HeapObjectKind::Environment,
            HeapObjectKind::Cell,
            HeapObjectKind::Array,
            HeapObjectKind::Map,
            HeapObjectKind::Struct,
        ] {
            assert_eq!(live.for_kind(kind).allocations, 1);
            assert_eq!(live.for_kind(kind).live, 1);
            assert_eq!(live.for_kind(kind).dead, 0);
        }

        drop(environment);
        drop(cell);
        drop(array);
        drop(map);
        drop(structure);

        let dead = stats.snapshot();
        assert_eq!(dead.total_allocations, 5);
        assert_eq!(dead.total_live, 0);
        assert_eq!(dead.total_dead, 5);
        assert_eq!(dead.peak_live, 5);
        assert_eq!(
            dead.for_kind(HeapObjectKind::Array).dead,
            dead.for_kind(HeapObjectKind::Array).allocations
        );
    }

    #[test]
    fn heap_stats_estimates_payload_capacity_and_releases_it() {
        let mut heap = Heap::new();
        let stats = heap.stats();
        let array = heap
            .allocate_array(vec![Value::number(1.0)])
            .expect("array identity should be available");

        let initial = stats.snapshot();
        let initial_bytes = initial.for_kind(HeapObjectKind::Array).estimated_bytes;
        assert!(initial_bytes > 0);

        if let Value::Array(array) = &array {
            array
                .elements
                .borrow_mut()
                .extend((0..256).map(|value| Value::number(value as f64)));
        } else {
            panic!("expected array payload");
        }

        let grown = stats.snapshot();
        let grown_array = grown.for_kind(HeapObjectKind::Array);
        assert!(grown_array.estimated_bytes > initial_bytes);
        assert_eq!(grown.estimated_live_bytes, grown_array.estimated_bytes);
        assert_eq!(grown.estimated_peak_live_bytes, grown_array.estimated_bytes);

        drop(array);
        let released = stats.snapshot();
        assert_eq!(released.estimated_live_bytes, 0);
        assert_eq!(
            released.estimated_peak_live_bytes,
            grown.estimated_peak_live_bytes
        );
        assert_eq!(released.for_kind(HeapObjectKind::Array).estimated_bytes, 0);
    }

    #[test]
    fn heap_stats_distinguishes_acyclic_storage_from_an_array_cycle() {
        let mut heap = Heap::new();
        let stats = heap.stats();
        let acyclic = heap
            .allocate_array(Vec::new())
            .expect("acyclic array identity should be available");
        let cycle = heap
            .allocate_array(Vec::new())
            .expect("cyclic array identity should be available");

        if let Value::Array(array) = &cycle {
            array.elements.borrow_mut().push(cycle.clone());
        } else {
            panic!("expected array cycle");
        }
        drop(acyclic);
        drop(cycle);

        let snapshot = stats.snapshot();
        let arrays = snapshot.for_kind(HeapObjectKind::Array);
        assert_eq!(arrays.allocations, 2);
        assert_eq!(arrays.live, 1);
        assert_eq!(arrays.dead, 1);
        assert_eq!(snapshot.peak_live, 2);
    }

    #[test]
    fn heap_stats_observes_a_closure_cell_cycle_without_displaying_it() {
        let mut heap = Heap::new();
        let stats = heap.stats();
        let environment = heap.new_environment();
        let cell = heap.new_cell(Value::Nil);
        environment
            .borrow_mut()
            .insert("closure".to_string(), cell.clone());
        let function = heap
            .allocate_function("cycle", 0, 0, environment.clone())
            .expect("function identity should be available");
        *cell.borrow_mut() = function;

        drop(environment);
        let cycle = stats.snapshot();
        assert_eq!(cycle.for_kind(HeapObjectKind::Environment).live, 1);
        assert_eq!(cycle.for_kind(HeapObjectKind::Cell).live, 1);
        assert_eq!(cycle.peak_live, 2);

        *cell.borrow_mut() = Value::Nil;
        let broken = stats.snapshot();
        assert_eq!(broken.for_kind(HeapObjectKind::Environment).live, 0);
        assert_eq!(broken.for_kind(HeapObjectKind::Cell).live, 1);
        drop(cell);
        assert_eq!(stats.snapshot().total_live, 0);
    }
}
