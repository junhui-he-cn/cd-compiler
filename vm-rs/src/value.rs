#![allow(dead_code)]

use crate::runtime::{ArrayValue, FunctionValue, MapValue, RangeValue, StructValue, VariantValue};
use std::collections::HashSet;
use std::fmt;
use std::rc::Rc;

#[derive(Clone, Debug)]
pub enum Value {
    Nil,
    Number(f64),
    Bool(bool),
    String(Rc<str>),
    Function(FunctionValue),
    Array(ArrayValue),
    Map(MapValue),
    Range(RangeValue),
    Struct(StructValue),
    Variant(VariantValue),
}

impl Value {
    pub fn number(value: f64) -> Self {
        Self::Number(value)
    }

    pub fn boolean(value: bool) -> Self {
        Self::Bool(value)
    }

    pub fn string(value: impl Into<Rc<str>>) -> Self {
        Self::String(value.into())
    }

    pub fn function(value: FunctionValue) -> Self {
        Self::Function(value)
    }

    pub fn array(value: ArrayValue) -> Self {
        Self::Array(value)
    }

    pub fn map(value: MapValue) -> Self {
        Self::Map(value)
    }

    pub fn range(value: RangeValue) -> Self {
        Self::Range(value)
    }

    pub fn structure(value: StructValue) -> Self {
        Self::Struct(value)
    }

    pub fn variant(value: VariantValue) -> Self {
        Self::Variant(value)
    }

    pub fn type_name(&self) -> &str {
        match self {
            Self::Nil => "nil",
            Self::Number(_) => "number",
            Self::Bool(_) => "bool",
            Self::String(_) => "string",
            Self::Function(_) => "function",
            Self::Array(_) => "array",
            Self::Map(_) => "map",
            Self::Range(_) => "range",
            Self::Struct(value) => value.type_name.as_deref().unwrap_or("struct"),
            Self::Variant(value) => &value.enum_name,
        }
    }

    pub fn is_truthy(&self) -> bool {
        !matches!(self, Self::Nil | Self::Bool(false))
    }

    pub fn runtime_equals(&self, other: &Self) -> bool {
        match (self, other) {
            (Self::Nil, Self::Nil) => true,
            (Self::Number(left), Self::Number(right)) => left == right,
            (Self::Bool(left), Self::Bool(right)) => left == right,
            (Self::String(left), Self::String(right)) => left == right,
            (Self::Function(left), Self::Function(right)) => left.identity == right.identity,
            (Self::Array(left), Self::Array(right)) => left.identity == right.identity,
            (Self::Map(left), Self::Map(right)) => left.identity == right.identity,
            (Self::Range(left), Self::Range(right)) => {
                left.start == right.start && left.stop == right.stop && left.step == right.step
            }
            (Self::Struct(left), Self::Struct(right)) => left.identity == right.identity,
            (Self::Variant(left), Self::Variant(right)) => {
                left.enum_name == right.enum_name
                    && left.variant_name == right.variant_name
                    && left.fields.len() == right.fields.len()
                    && left
                        .fields
                        .iter()
                        .zip(right.fields.iter())
                        .all(|(left, right)| left.runtime_equals(right))
            }
            _ => false,
        }
    }

    pub fn runtime_hash(&self) -> f64 {
        let mut hash = Fnv1a32::new();
        hash_value_into(&mut hash, self);
        hash.finish() as f64
    }
}

struct Fnv1a32 {
    hash: u32,
}

impl Fnv1a32 {
    fn new() -> Self {
        Self {
            hash: 2_166_136_261,
        }
    }

    fn byte(&mut self, value: u8) {
        self.hash ^= u32::from(value);
        self.hash = self.hash.wrapping_mul(16_777_619);
    }

    fn number(&mut self, value: u64) {
        for byte in value.to_le_bytes() {
            self.byte(byte);
        }
    }

    fn text(&mut self, value: &str) {
        self.number(value.len() as u64);
        for byte in value.as_bytes() {
            self.byte(*byte);
        }
    }

    fn finish(&self) -> u32 {
        self.hash
    }
}

fn hash_value_into(hash: &mut Fnv1a32, value: &Value) {
    hash.byte(match value {
        Value::Nil => 0,
        Value::Number(_) => 1,
        Value::Bool(_) => 2,
        Value::String(_) => 3,
        Value::Function(_) => 4,
        Value::Array(_) => 5,
        Value::Map(_) => 6,
        Value::Range(_) => 7,
        Value::Struct(_) => 8,
        Value::Variant(_) => 9,
    });

    match value {
        Value::Nil => {}
        Value::Number(number) => {
            let bits = if number.is_nan() {
                0x7ff8_0000_0000_0000
            } else if *number == 0.0 {
                0
            } else {
                number.to_bits()
            };
            hash.number(bits);
        }
        Value::Bool(value) => hash.byte(u8::from(*value)),
        Value::String(value) => hash.text(value),
        Value::Function(value) => hash.number(value.identity as u64),
        Value::Array(value) => hash.number(value.identity as u64),
        Value::Map(value) => hash.number(value.identity as u64),
        Value::Range(value) => {
            hash.number(value.start as u64);
            hash.number(value.stop as u64);
            hash.number(value.step as u64);
        }
        Value::Struct(value) => hash.number(value.identity as u64),
        Value::Variant(value) => {
            hash.text(&value.enum_name);
            hash.text(&value.variant_name);
            hash.number(value.fields.len() as u64);
            for field in &value.fields {
                hash_value_into(hash, field);
            }
        }
    }
}

fn format_number(value: f64) -> String {
    if value.fract() == 0.0 {
        format!("{:.0}", value)
    } else {
        value.to_string()
    }
}

fn format_value(value: &Value, active_references: &mut HashSet<(u8, usize)>) -> String {
    match value {
        Value::Nil => "nil".to_string(),
        Value::Number(value) => format_number(*value),
        Value::Bool(value) => if *value { "true" } else { "false" }.to_string(),
        Value::String(value) => value.to_string(),
        Value::Function(function) => format!("<fn {}>", function.name),
        Value::Array(array) => {
            let reference = (0, array.identity);
            if !active_references.insert(reference) {
                return "<cycle>".to_string();
            }
            let elements = array.elements.borrow();
            let mut output = String::from("[");
            for (index, element) in elements.iter().enumerate() {
                if index != 0 {
                    output.push_str(", ");
                }
                output.push_str(&format_value(element, active_references));
            }
            output.push(']');
            active_references.remove(&reference);
            output
        }
        Value::Map(map) => {
            let reference = (1, map.identity);
            if !active_references.insert(reference) {
                return "<cycle>".to_string();
            }
            let entries = map.entries.borrow();
            let mut output = String::from("map{");
            for (index, (key, value)) in entries.iter().enumerate() {
                if index != 0 {
                    output.push_str(", ");
                }
                output.push_str(&format_value(key, active_references));
                output.push_str(": ");
                output.push_str(&format_value(value, active_references));
            }
            output.push('}');
            active_references.remove(&reference);
            output
        }
        Value::Range(range) => format!("range({}, {}, {})", range.start, range.stop, range.step),
        Value::Struct(value) => {
            let reference = (2, value.identity);
            if !active_references.insert(reference) {
                return "<cycle>".to_string();
            }
            let fields = value.fields.borrow();
            let mut output = String::from("{");
            for (index, (name, field_value)) in fields.iter().enumerate() {
                if index != 0 {
                    output.push_str(", ");
                }
                output.push_str(name);
                output.push_str(": ");
                output.push_str(&format_value(field_value, active_references));
            }
            output.push('}');
            active_references.remove(&reference);
            output
        }
        Value::Variant(value) => {
            let mut output = format!("{}.{}", value.enum_name, value.variant_name);
            if !value.fields.is_empty() {
                output.push('(');
                for (index, field) in value.fields.iter().enumerate() {
                    if index != 0 {
                        output.push_str(", ");
                    }
                    output.push_str(&format_value(field, active_references));
                }
                output.push(')');
            }
            output
        }
    }
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut active_references = HashSet::new();
        f.write_str(&format_value(self, &mut active_references))
    }
}

#[cfg(test)]
mod tests {
    use super::Value;
    use crate::runtime::{Heap, VariantValue};

    #[test]
    fn formats_primitives_like_cpp_runtime() {
        assert_eq!(Value::Nil.to_string(), "nil");
        assert_eq!(Value::number(7.0).to_string(), "7");
        assert_eq!(Value::number(1.25).to_string(), "1.25");
        assert_eq!(Value::boolean(true).to_string(), "true");
        assert_eq!(Value::boolean(false).to_string(), "false");
        assert_eq!(Value::string("hello").to_string(), "hello");
    }

    #[test]
    fn formats_recursive_structs_with_a_cycle_marker() {
        let mut heap = Heap::new();
        let node = heap
            .allocate_struct(Some("Node".to_string()), Vec::new())
            .expect("node identity should be available");
        let Value::Struct(struct_value) = &node else {
            panic!("expected struct value");
        };
        struct_value
            .fields
            .borrow_mut()
            .push(("next".to_string(), node.clone()));

        assert!(node.runtime_equals(&node));
        assert_eq!(node.to_string(), "{next: <cycle>}");
    }

    #[test]
    fn formats_repeated_struct_aliases_without_false_cycle_markers() {
        let mut heap = Heap::new();
        let child = heap
            .allocate_struct(Some("Node".to_string()), vec![("value".to_string(), Value::number(1.0))])
            .expect("child identity should be available");
        let parent = heap
            .allocate_struct(
                Some("Pair".to_string()),
                vec![
                    ("left".to_string(), child.clone()),
                    ("right".to_string(), child),
                ],
            )
            .expect("parent identity should be available");

        assert_eq!(parent.to_string(), "{left: {value: 1}, right: {value: 1}}");
    }

    #[test]
    fn truthiness_matches_language_runtime() {
        assert!(!Value::Nil.is_truthy());
        assert!(!Value::boolean(false).is_truthy());
        assert!(Value::boolean(true).is_truthy());
        assert!(Value::number(0.0).is_truthy());
        assert!(Value::string("").is_truthy());
    }

    #[test]
    fn primitive_equality_matches_runtime() {
        assert!(Value::Nil.runtime_equals(&Value::Nil));
        assert!(Value::number(2.0).runtime_equals(&Value::number(2.0)));
        assert!(!Value::number(2.0).runtime_equals(&Value::number(3.0)));
        assert!(Value::boolean(true).runtime_equals(&Value::boolean(true)));
        assert!(Value::string("x").runtime_equals(&Value::string("x")));
        assert!(!Value::string("x").runtime_equals(&Value::number(0.0)));
    }

    #[test]
    fn primitive_hash_matches_the_cpp_value_contract() {
        assert_eq!(Value::Nil.runtime_hash(), 84_696_351.0);
        assert_eq!(Value::number(42.0).runtime_hash(), 1_983_088_465.0);
        assert_eq!(Value::boolean(true).runtime_hash(), 1_551_600_396.0);
        assert_eq!(Value::string("hello").runtime_hash(), 910_946_861.0);
        assert_eq!(
            Value::number(-0.0).runtime_hash(),
            Value::number(0.0).runtime_hash()
        );
        assert_ne!(
            Value::string("hello").runtime_hash(),
            Value::string("world").runtime_hash()
        );
    }

    #[test]
    fn enum_variants_format_and_compare_structurally() {
        let left = Value::variant(VariantValue {
            enum_name: "Result".to_string(),
            variant_name: "Ok".to_string(),
            fields: vec![Value::number(7.0)],
        });
        let right = Value::variant(VariantValue {
            enum_name: "Result".to_string(),
            variant_name: "Ok".to_string(),
            fields: vec![Value::number(7.0)],
        });
        let other = Value::variant(VariantValue {
            enum_name: "Result".to_string(),
            variant_name: "Err".to_string(),
            fields: vec![Value::string("bad")],
        });

        assert_eq!(left.type_name(), "Result");
        assert_eq!(left.to_string(), "Result.Ok(7)");
        assert!(left.runtime_equals(&right));
        assert!(!left.runtime_equals(&other));
    }
}
