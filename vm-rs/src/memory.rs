//! Deterministic VM virtual-address space for cdbc 0.3 machine objects.
//!
//! The backing vector is an implementation detail. Callers may only access it
//! through checked VM addresses; a VM address is never a host pointer.

use std::fmt;
use std::ops::Range;

/// A byte address in the VM virtual address space.
pub type VmAddress = u64;

/// Address zero is the null pointer.
pub const NULL_ADDRESS: VmAddress = 0;
/// The half-open null/invalid guard occupies `[0, NULL_GUARD_END)`.
pub const NULL_GUARD_END: VmAddress = 0x1000;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum MemoryRegionKind {
    Rodata,
    Data,
    Bss,
    Heap,
    Stack,
}

impl MemoryRegionKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Rodata => "rodata",
            Self::Data => "data",
            Self::Bss => "bss",
            Self::Heap => "heap",
            Self::Stack => "stack",
        }
    }

    pub const fn permissions(self) -> MemoryPermissions {
        match self {
            Self::Rodata => MemoryPermissions::ReadOnly,
            Self::Data | Self::Bss | Self::Heap | Self::Stack => MemoryPermissions::ReadWrite,
        }
    }
}

impl fmt::Display for MemoryRegionKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemoryPermissions {
    ReadOnly,
    ReadWrite,
}

impl MemoryPermissions {
    pub const fn is_writable(self) -> bool {
        matches!(self, Self::ReadWrite)
    }

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::ReadOnly => "read-only",
            Self::ReadWrite => "read-write",
        }
    }
}

/// Metadata for one mapped half-open byte interval `[base, end)`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MemoryRegion {
    pub kind: MemoryRegionKind,
    pub base: VmAddress,
    pub end: VmAddress,
    pub alignment: VmAddress,
    pub permissions: MemoryPermissions,
}

impl MemoryRegion {
    pub const fn size(self) -> VmAddress {
        self.end.saturating_sub(self.base)
    }

    pub const fn contains(self, address: VmAddress) -> bool {
        self.base <= address && address < self.end
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MemoryErrorKind {
    InvalidAlignment,
    RegionOverlap,
    AllocationFailure,
    NullPointerAccess,
    InvalidAddress,
    MemoryOutOfBounds,
    WriteToReadOnlyMemory,
}

impl MemoryErrorKind {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::InvalidAlignment => "invalid_alignment",
            Self::RegionOverlap => "region_overlap",
            Self::AllocationFailure => "allocation_failure",
            Self::NullPointerAccess => "null_pointer_access",
            Self::InvalidAddress => "invalid_address",
            Self::MemoryOutOfBounds => "memory_out_of_bounds",
            Self::WriteToReadOnlyMemory => "write_to_read_only_memory",
        }
    }
}

impl fmt::Display for MemoryErrorKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MemoryError {
    InvalidAlignment {
        alignment: VmAddress,
    },
    RegionOverlap {
        base: VmAddress,
        end: VmAddress,
    },
    AllocationFailure {
        requested_bytes: usize,
    },
    NullPointerAccess {
        size: usize,
    },
    InvalidAddress {
        address: VmAddress,
        size: usize,
    },
    MemoryOutOfBounds {
        address: VmAddress,
        size: usize,
        region_end: VmAddress,
    },
    WriteToReadOnlyMemory {
        address: VmAddress,
        size: usize,
        region: MemoryRegionKind,
    },
}

impl MemoryError {
    pub const fn kind(&self) -> MemoryErrorKind {
        match self {
            Self::InvalidAlignment { .. } => MemoryErrorKind::InvalidAlignment,
            Self::RegionOverlap { .. } => MemoryErrorKind::RegionOverlap,
            Self::AllocationFailure { .. } => MemoryErrorKind::AllocationFailure,
            Self::NullPointerAccess { .. } => MemoryErrorKind::NullPointerAccess,
            Self::InvalidAddress { .. } => MemoryErrorKind::InvalidAddress,
            Self::MemoryOutOfBounds { .. } => MemoryErrorKind::MemoryOutOfBounds,
            Self::WriteToReadOnlyMemory { .. } => MemoryErrorKind::WriteToReadOnlyMemory,
        }
    }
}

impl fmt::Display for MemoryError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidAlignment { alignment } => {
                write!(formatter, "memory alignment {} is not a positive power of two", alignment)
            }
            Self::RegionOverlap { base, end } => write!(
                formatter,
                "memory region [0x{base:016x}, 0x{end:016x}) overlaps an existing region"
            ),
            Self::AllocationFailure { requested_bytes } => write!(
                formatter,
                "linear memory allocation failed for {} bytes",
                requested_bytes
            ),
            Self::NullPointerAccess { size } => {
                write!(formatter, "null pointer access for {} bytes", size)
            }
            Self::InvalidAddress { address, size } => write!(
                formatter,
                "invalid VM address 0x{address:016x} for {} bytes",
                size
            ),
            Self::MemoryOutOfBounds {
                address,
                size,
                region_end,
            } => write!(
                formatter,
                "memory access at 0x{address:016x} for {} bytes exceeds region end 0x{region_end:016x}",
                size
            ),
            Self::WriteToReadOnlyMemory {
                address,
                size,
                region,
            } => write!(
                formatter,
                "write of {} bytes at 0x{address:016x} is not permitted in {region}",
                size
            ),
        }
    }
}

impl std::error::Error for MemoryError {}

/// Checked, byte-addressable VM memory.
#[derive(Clone, Debug)]
pub struct LinearMemory {
    // Keep each mapped region's bytes separate so sparse VM addresses do not
    // require materializing the unmapped gaps between regions.
    backings: Vec<Vec<u8>>,
    regions: Vec<MemoryRegion>,
    next_address: VmAddress,
}

impl Default for LinearMemory {
    fn default() -> Self {
        Self::new()
    }
}

impl LinearMemory {
    pub fn new() -> Self {
        Self {
            backings: Vec::new(),
            regions: Vec::new(),
            next_address: NULL_GUARD_END,
        }
    }

    /// The next cursor used by deterministic sequential allocation.
    pub const fn next_address(&self) -> VmAddress {
        self.next_address
    }

    /// The mapped intervals in ascending VM address order.
    pub fn regions(&self) -> &[MemoryRegion] {
        &self.regions
    }

    pub fn region_for_address(&self, address: VmAddress) -> Option<&MemoryRegion> {
        self.regions.iter().find(|region| region.contains(address))
    }

    pub fn is_mapped(&self, address: VmAddress) -> bool {
        self.region_for_address(address).is_some()
    }

    /// Allocate the next region using checked alignment and address arithmetic.
    /// Region permissions are implied by `kind`.
    pub fn allocate_region(
        &mut self,
        kind: MemoryRegionKind,
        size: VmAddress,
        alignment: VmAddress,
    ) -> Result<MemoryRegion, MemoryError> {
        validate_alignment(alignment)?;
        let base = align_up(self.next_address, alignment)?;
        self.map_region_at_zeroed(kind, base, size, alignment)
    }

    /// Allocate an initialized region. The payload length is the region size.
    pub fn allocate_region_with_bytes(
        &mut self,
        kind: MemoryRegionKind,
        alignment: VmAddress,
        initial: &[u8],
    ) -> Result<MemoryRegion, MemoryError> {
        validate_alignment(alignment)?;
        let base = align_up(self.next_address, alignment)?;
        self.map_region_at_with_bytes(kind, base, alignment, initial)
    }

    /// Map a zero-initialized region at an explicit VM address.
    ///
    /// This is useful for loaders and tests that need to construct a known
    /// address layout. Non-empty regions must lie outside the null guard and
    /// must not overlap any existing region.
    pub fn map_region_at(
        &mut self,
        kind: MemoryRegionKind,
        base: VmAddress,
        size: VmAddress,
        alignment: VmAddress,
    ) -> Result<MemoryRegion, MemoryError> {
        self.map_region_at_zeroed(kind, base, size, alignment)
    }

    fn map_region_at_zeroed(
        &mut self,
        kind: MemoryRegionKind,
        base: VmAddress,
        size: VmAddress,
        alignment: VmAddress,
    ) -> Result<MemoryRegion, MemoryError> {
        self.map_region_at_impl(kind, base, size, alignment, None)
    }

    /// Map an initialized region at an explicit VM address.
    pub fn map_region_at_with_bytes(
        &mut self,
        kind: MemoryRegionKind,
        base: VmAddress,
        alignment: VmAddress,
        initial: &[u8],
    ) -> Result<MemoryRegion, MemoryError> {
        let size = u64::try_from(initial.len()).map_err(|_| MemoryError::InvalidAddress {
            address: base,
            size: initial.len(),
        })?;
        self.map_region_at_impl(kind, base, size, alignment, Some(initial))
    }

    fn map_region_at_impl(
        &mut self,
        kind: MemoryRegionKind,
        base: VmAddress,
        size: VmAddress,
        alignment: VmAddress,
        initial: Option<&[u8]>,
    ) -> Result<MemoryRegion, MemoryError> {
        validate_alignment(alignment)?;
        if base % alignment != 0 {
            return Err(MemoryError::InvalidAlignment { alignment });
        }
        let end = base.checked_add(size).ok_or(MemoryError::InvalidAddress {
            address: base,
            size: usize::try_from(size).unwrap_or(usize::MAX),
        })?;
        if size != 0 && base < NULL_GUARD_END {
            return Err(MemoryError::InvalidAddress {
                address: base,
                size: usize::try_from(size).unwrap_or(usize::MAX),
            });
        }
        if size != 0
            && self
                .regions
                .iter()
                .any(|region| base < region.end && region.base < end)
        {
            return Err(MemoryError::RegionOverlap { base, end });
        }

        let backing = if size == 0 {
            None
        } else {
            let mut backing = allocate_backing(size)?;
            if let Some(initial) = initial {
                backing.copy_from_slice(initial);
            }
            Some(backing)
        };

        let region = MemoryRegion {
            kind,
            base,
            end,
            alignment,
            permissions: kind.permissions(),
        };
        if let Some(backing) = backing {
            let index = self
                .regions
                .binary_search_by_key(&base, |candidate| candidate.base)
                .unwrap_or_else(|index| index);
            self.regions.insert(index, region);
            self.backings.insert(index, backing);
        }
        self.next_address = self.next_address.max(end);
        Ok(region)
    }

    pub fn read_bytes(&self, address: VmAddress, size: usize) -> Result<&[u8], MemoryError> {
        if size == 0 {
            return Ok(&[]);
        }
        let (region_index, range) = self.checked_range(address, size, false)?;
        self.backings
            .get(region_index)
            .and_then(|backing| backing.get(range))
            .ok_or(MemoryError::InvalidAddress { address, size })
    }

    pub fn write_bytes(&mut self, address: VmAddress, bytes: &[u8]) -> Result<(), MemoryError> {
        let size = bytes.len();
        if size == 0 {
            return Ok(());
        }
        let (region_index, range) = self.checked_range(address, size, true)?;
        let Some(destination) = self
            .backings
            .get_mut(region_index)
            .and_then(|backing| backing.get_mut(range))
        else {
            return Err(MemoryError::InvalidAddress { address, size });
        };
        destination.copy_from_slice(bytes);
        Ok(())
    }

    pub fn fill_bytes(
        &mut self,
        address: VmAddress,
        size: usize,
        value: u8,
    ) -> Result<(), MemoryError> {
        if size == 0 {
            return Ok(());
        }
        let (region_index, range) = self.checked_range(address, size, true)?;
        let Some(destination) = self
            .backings
            .get_mut(region_index)
            .and_then(|backing| backing.get_mut(range))
        else {
            return Err(MemoryError::InvalidAddress { address, size });
        };
        destination.fill(value);
        Ok(())
    }

    pub fn read(&self, address: VmAddress, size: usize) -> Result<&[u8], MemoryError> {
        self.read_bytes(address, size)
    }

    pub fn write(&mut self, address: VmAddress, bytes: &[u8]) -> Result<(), MemoryError> {
        self.write_bytes(address, bytes)
    }

    fn checked_range(
        &self,
        address: VmAddress,
        size: usize,
        write: bool,
    ) -> Result<(usize, Range<usize>), MemoryError> {
        if address == NULL_ADDRESS {
            return Err(MemoryError::NullPointerAccess { size });
        }
        let size_u64 =
            u64::try_from(size).map_err(|_| MemoryError::InvalidAddress { address, size })?;
        let end = address
            .checked_add(size_u64)
            .ok_or(MemoryError::InvalidAddress { address, size })?;
        let Some(region) = self.region_for_address(address).copied() else {
            return Err(MemoryError::InvalidAddress { address, size });
        };
        if end > region.end {
            return Err(MemoryError::MemoryOutOfBounds {
                address,
                size,
                region_end: region.end,
            });
        }
        if write && !region.permissions.is_writable() {
            return Err(MemoryError::WriteToReadOnlyMemory {
                address,
                size,
                region: region.kind,
            });
        }
        let region_index = self
            .regions
            .iter()
            .position(|candidate| candidate.contains(address))
            .ok_or(MemoryError::InvalidAddress { address, size })?;
        let start = usize::try_from(address - region.base)
            .map_err(|_| MemoryError::InvalidAddress { address, size })?;
        let end = start
            .checked_add(size)
            .ok_or(MemoryError::InvalidAddress { address, size })?;
        Ok((region_index, start..end))
    }
}

fn validate_alignment(alignment: VmAddress) -> Result<(), MemoryError> {
    if alignment == 0 || !alignment.is_power_of_two() {
        return Err(MemoryError::InvalidAlignment { alignment });
    }
    Ok(())
}

fn align_up(address: VmAddress, alignment: VmAddress) -> Result<VmAddress, MemoryError> {
    let mask = alignment - 1;
    let remainder = address & mask;
    let padding = if remainder == 0 {
        0
    } else {
        alignment - remainder
    };
    address
        .checked_add(padding)
        .ok_or(MemoryError::InvalidAddress { address, size: 0 })
}

fn allocate_backing(size: VmAddress) -> Result<Vec<u8>, MemoryError> {
    let byte_len = usize::try_from(size).map_err(|_| MemoryError::AllocationFailure {
        requested_bytes: usize::MAX,
    })?;
    let mut backing = Vec::new();
    backing
        .try_reserve_exact(byte_len)
        .map_err(|_| MemoryError::AllocationFailure {
            requested_bytes: byte_len,
        })?;
    backing.resize(byte_len, 0);
    Ok(backing)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allocation_is_deterministic_and_obeys_alignment() {
        let allocate = || {
            let mut memory = LinearMemory::new();
            let rodata = memory
                .allocate_region_with_bytes(MemoryRegionKind::Rodata, 1, b"abc")
                .expect("rodata allocation");
            let data = memory
                .allocate_region(MemoryRegionKind::Data, 4, 4)
                .expect("data allocation");
            let bss = memory
                .allocate_region(MemoryRegionKind::Bss, 5, 8)
                .expect("bss allocation");
            (memory, rodata, data, bss)
        };

        let (first, rodata, data, bss) = allocate();
        let (second, second_rodata, second_data, second_bss) = allocate();
        assert_eq!(first.regions(), second.regions());
        assert_eq!(rodata.base, 0x1000);
        assert_eq!(rodata.end, 0x1003);
        assert_eq!(data.base, 0x1004);
        assert_eq!(bss.base, 0x1008);
        assert_eq!(first.next_address(), 0x100d);
        assert_eq!(rodata, second_rodata);
        assert_eq!(data, second_data);
        assert_eq!(bss, second_bss);
        assert_eq!(first.read_bytes(rodata.base, 3).unwrap(), b"abc");
        assert_eq!(first.read_bytes(bss.base, 5).unwrap(), &[0, 0, 0, 0, 0]);
    }

    #[test]
    fn rejects_invalid_alignment_and_address_overflow() {
        let mut memory = LinearMemory::new();
        assert_eq!(
            memory
                .allocate_region(MemoryRegionKind::Data, 1, 3)
                .unwrap_err()
                .kind(),
            MemoryErrorKind::InvalidAlignment
        );
        assert_eq!(
            memory
                .map_region_at(MemoryRegionKind::Data, u64::MAX - 1, 2, 1)
                .unwrap_err()
                .kind(),
            MemoryErrorKind::InvalidAddress
        );
    }

    #[test]
    fn distinguishes_null_unmapped_and_overlong_ranges() {
        let mut memory = LinearMemory::new();
        let region = memory
            .allocate_region(MemoryRegionKind::Data, 4, 1)
            .expect("data allocation");

        assert_eq!(
            memory.read_bytes(NULL_ADDRESS, 1).unwrap_err().kind(),
            MemoryErrorKind::NullPointerAccess
        );
        assert_eq!(
            memory.read_bytes(region.base + 8, 1).unwrap_err().kind(),
            MemoryErrorKind::InvalidAddress
        );
        assert_eq!(
            memory.read_bytes(region.base, 5).unwrap_err().kind(),
            MemoryErrorKind::MemoryOutOfBounds
        );
        assert_eq!(
            memory.read_bytes(u64::MAX, 2).unwrap_err().kind(),
            MemoryErrorKind::InvalidAddress
        );
    }

    #[test]
    fn ranges_do_not_cross_adjacent_regions() {
        let mut memory = LinearMemory::new();
        let first = memory
            .allocate_region(MemoryRegionKind::Data, 4, 1)
            .expect("first region");
        let second = memory
            .allocate_region(MemoryRegionKind::Data, 4, 1)
            .expect("second region");
        assert_eq!(first.end, second.base);
        assert_eq!(
            memory.read_bytes(first.base, 5).unwrap_err().kind(),
            MemoryErrorKind::MemoryOutOfBounds
        );
    }

    #[test]
    fn allows_unaligned_access_and_checks_read_only_writes() {
        let mut memory = LinearMemory::new();
        let data = memory
            .allocate_region(MemoryRegionKind::Data, 8, 8)
            .expect("data allocation");
        memory
            .write_bytes(data.base + 1, &[1, 2, 3, 4])
            .expect("unaligned write");
        assert_eq!(memory.read_bytes(data.base + 1, 4).unwrap(), &[1, 2, 3, 4]);

        let rodata = memory
            .allocate_region_with_bytes(MemoryRegionKind::Rodata, 1, b"read-only")
            .expect("rodata allocation");
        assert_eq!(
            memory.write_bytes(rodata.base, &[0]).unwrap_err().kind(),
            MemoryErrorKind::WriteToReadOnlyMemory
        );
    }

    #[test]
    fn failed_writes_leave_existing_bytes_unchanged() {
        let mut memory = LinearMemory::new();
        let data = memory
            .allocate_region_with_bytes(MemoryRegionKind::Data, 1, &[1, 2, 3, 4])
            .expect("data allocation");

        assert_eq!(
            memory.write_bytes(data.base, &[9, 8, 7, 6, 5]).unwrap_err(),
            MemoryError::MemoryOutOfBounds {
                address: data.base,
                size: 5,
                region_end: data.end,
            }
        );
        assert_eq!(memory.read_bytes(data.base, 4).unwrap(), &[1, 2, 3, 4]);
    }

    #[test]
    fn zero_length_operations_do_not_dereference_addresses() {
        let mut memory = LinearMemory::new();
        assert_eq!(memory.read_bytes(NULL_ADDRESS, 0).unwrap(), &[]);
        memory
            .write_bytes(NULL_ADDRESS, &[])
            .expect("null zero-length write is a no-op");
        memory
            .fill_bytes(u64::MAX, 0, 0xff)
            .expect("unmapped zero-length fill is a no-op");
    }

    #[test]
    fn rejects_overlapping_explicit_regions() {
        let mut memory = LinearMemory::new();
        memory
            .map_region_at(MemoryRegionKind::Data, 0x2000, 8, 8)
            .expect("first explicit region");
        assert_eq!(
            memory
                .map_region_at(MemoryRegionKind::Bss, 0x2004, 4, 4)
                .unwrap_err()
                .kind(),
            MemoryErrorKind::RegionOverlap
        );
    }

    #[test]
    fn sparse_explicit_addresses_do_not_materialize_unmapped_gaps() {
        let mut memory = LinearMemory::new();
        let base = 0x1_0000_0000u64;
        let region = memory
            .map_region_at_with_bytes(MemoryRegionKind::Data, base, 8, &[4, 5, 6])
            .expect("high explicit region");

        assert_eq!(region.base, base);
        assert_eq!(memory.read_bytes(base, 3).unwrap(), &[4, 5, 6]);
        assert_eq!(memory.regions(), &[region]);

        let next = memory
            .allocate_region(MemoryRegionKind::Bss, 2, 1)
            .expect("sequential region after explicit mapping");
        assert_eq!(next.base, base + 3);
    }
}
