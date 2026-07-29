#![allow(dead_code)]

use crate::value::Value;
use std::cell::RefCell;
use std::collections::HashMap;
use std::fmt;
use std::rc::Rc;
use std::rc::Weak;

pub type Cell = Rc<RefCell<Value>>;
pub type Environment = HashMap<String, Cell>;
pub type SharedEnvironment = Rc<RefCell<Environment>>;
pub type SharedArrayElements = Rc<RefCell<Vec<Value>>>;
pub type SharedMapEntries = Rc<RefCell<Vec<(Value, Value)>>>;
pub type SharedStructFields = Rc<RefCell<Vec<(String, Value)>>>;

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
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HeapStatsSnapshot {
    by_kind: [HeapObjectStats; HeapObjectKind::COUNT],
    pub total_allocations: usize,
    pub total_live: usize,
    pub total_dead: usize,
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
        self.ledger.borrow().snapshot()
    }
}

#[derive(Debug)]
enum WeakAllocation {
    Environment(Weak<RefCell<Environment>>),
    Cell(Weak<RefCell<Value>>),
    Array(Weak<RefCell<Vec<Value>>>),
    Map(Weak<RefCell<Vec<(Value, Value)>>>),
    Struct(Weak<RefCell<Vec<(String, Value)>>>),
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

    fn is_live(&self) -> bool {
        match self {
            Self::Environment(value) => value.strong_count() != 0,
            Self::Cell(value) => value.strong_count() != 0,
            Self::Array(value) => value.strong_count() != 0,
            Self::Map(value) => value.strong_count() != 0,
            Self::Struct(value) => value.strong_count() != 0,
        }
    }
}

#[derive(Debug, Default)]
struct HeapLedger {
    allocations: Vec<WeakAllocation>,
}

impl HeapLedger {
    fn record(&mut self, allocation: WeakAllocation) {
        self.allocations.push(allocation);
    }

    fn snapshot(&self) -> HeapStatsSnapshot {
        let mut by_kind = [HeapObjectStats::default(); HeapObjectKind::COUNT];
        for allocation in &self.allocations {
            let kind = allocation.kind();
            let stats = &mut by_kind[kind.index()];
            stats.allocations += 1;
            if allocation.is_live() {
                stats.live += 1;
            }
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
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HeapError;

impl fmt::Display for HeapError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("runtime identity exhausted")
    }
}

/// The first VM-2B ownership boundary.
///
/// This facade deliberately retains the current reference-counted storage. It
/// owns VM-local identity allocation and the construction of runtime storage;
/// VM execution remains responsible for resource-budget charging and root
/// ownership until a later slice adds explicit heap accounting or handles.
#[derive(Debug)]
pub struct Heap {
    next_function_identity: usize,
    next_array_identity: usize,
    next_map_identity: usize,
    next_struct_identity: usize,
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
            ledger: Rc::new(RefCell::new(HeapLedger::default())),
        }
    }

    pub fn stats(&self) -> HeapStats {
        HeapStats {
            ledger: self.ledger.clone(),
        }
    }

    pub fn new_environment(&self) -> SharedEnvironment {
        let environment = Rc::new(RefCell::new(HashMap::new()));
        self.ledger
            .borrow_mut()
            .record(WeakAllocation::Environment(Rc::downgrade(&environment)));
        environment
    }

    pub fn new_cell(&self, value: Value) -> Cell {
        let cell = Rc::new(RefCell::new(value));
        self.ledger
            .borrow_mut()
            .record(WeakAllocation::Cell(Rc::downgrade(&cell)));
        cell
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
        let elements = Rc::new(RefCell::new(elements));
        self.ledger
            .borrow_mut()
            .record(WeakAllocation::Array(Rc::downgrade(&elements)));
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
        let entries = Rc::new(RefCell::new(ordered));
        self.ledger
            .borrow_mut()
            .record(WeakAllocation::Map(Rc::downgrade(&entries)));
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
        let fields = Rc::new(RefCell::new(fields));
        self.ledger
            .borrow_mut()
            .record(WeakAllocation::Struct(Rc::downgrade(&fields)));
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
        assert_eq!(
            dead.for_kind(HeapObjectKind::Array).dead,
            dead.for_kind(HeapObjectKind::Array).allocations
        );
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

        *cell.borrow_mut() = Value::Nil;
        let broken = stats.snapshot();
        assert_eq!(broken.for_kind(HeapObjectKind::Environment).live, 0);
        assert_eq!(broken.for_kind(HeapObjectKind::Cell).live, 1);
        drop(cell);
        assert_eq!(stats.snapshot().total_live, 0);
    }
}
