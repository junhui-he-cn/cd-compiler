#![allow(dead_code)]

use crate::value::Value;
use std::cell::RefCell;
use std::collections::HashMap;
use std::fmt;
use std::rc::Rc;

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
        }
    }

    pub fn new_environment(&self) -> SharedEnvironment {
        Rc::new(RefCell::new(HashMap::new()))
    }

    pub fn new_cell(&self, value: Value) -> Cell {
        Rc::new(RefCell::new(value))
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
        Ok(Value::array(ArrayValue {
            identity,
            elements: Rc::new(RefCell::new(elements)),
        }))
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
        Ok(Value::map(MapValue {
            identity,
            entries: Rc::new(RefCell::new(ordered)),
        }))
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
        Ok(Value::structure(StructValue {
            identity,
            type_name,
            fields: Rc::new(RefCell::new(fields)),
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
    use super::Heap;
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
}
