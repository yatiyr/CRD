//! The raptorq crate behind a C interface (tests/numerics/hesap-net/raptorq_peers.h declares it): a block of `k` symbols
//! of `t` octets as one source block, one sub-block, alignment 1.

use raptorq::{
    EncodingPacket, ObjectTransmissionInformation, PayloadId, SourceBlockDecoder, SourceBlockEncoder,
    SourceBlockEncodingPlan,
};
use std::slice;

fn config(k: u32, t: u32) -> ObjectTransmissionInformation {
    ObjectTransmissionInformation::new(u64::from(k) * u64::from(t), t as u16, 1, 1, 1)
}

/// A plan for blocks of `k` symbols, to give back to `crd_raptorq_crate_plan_free`.
#[unsafe(no_mangle)]
pub extern "C" fn crd_raptorq_crate_plan_new(k: u32) -> *mut SourceBlockEncodingPlan {
    Box::into_raw(Box::new(SourceBlockEncodingPlan::generate(k as u16)))
}

/// # Safety
/// `plan` came from `crd_raptorq_crate_plan_new` and is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn crd_raptorq_crate_plan_free(plan: *mut SourceBlockEncodingPlan) {
    drop(unsafe { Box::from_raw(plan) });
}

/// Encodes the block at `source` under `plan`, then writes `count` repair symbols of `t` octets to `out`, the repair
/// symbol numbers `first` onwards (ESI k + number).
///
/// # Safety
/// `plan` is live, `source` holds `k * t` octets and `out` holds `count * t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn crd_raptorq_crate_repair(
    plan: *const SourceBlockEncodingPlan,
    source: *const u8,
    k: u32,
    t: u32,
    first: u32,
    count: u32,
    out: *mut u8,
) {
    let data = unsafe { slice::from_raw_parts(source, k as usize * t as usize) };
    let encoder = SourceBlockEncoder::with_encoding_plan(0, &config(k, t), data, unsafe { &*plan });
    let packets = encoder.repair_packets(first, count);
    let output = unsafe { slice::from_raw_parts_mut(out, count as usize * t as usize) };
    for (packet, symbol) in packets.iter().zip(output.chunks_exact_mut(t as usize)) {
        symbol.copy_from_slice(packet.data());
    }
}

/// Decodes the block from `count` encoding symbols of `t` octets, `symbols[i]` carrying ESI `esis[i]`, into `out`, which
/// holds `k * t` octets. Whether the crate recovered it.
///
/// # Safety
/// `esis` and `symbols` hold `count` entries, each symbol `t` octets, and `out` holds `k * t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn crd_raptorq_crate_decode(
    k: u32,
    t: u32,
    esis: *const u32,
    symbols: *const *const u8,
    count: u32,
    out: *mut u8,
) -> bool {
    let esis = unsafe { slice::from_raw_parts(esis, count as usize) };
    let symbols = unsafe { slice::from_raw_parts(symbols, count as usize) };
    let packets = esis.iter().zip(symbols).map(|(&esi, &symbol)| {
        let data = unsafe { slice::from_raw_parts(symbol, t as usize) };
        EncodingPacket::new(PayloadId::new(0, esi), data.to_vec())
    });
    let block_length = u64::from(k) * u64::from(t);
    let mut decoder = SourceBlockDecoder::new(0, &config(k, t), block_length);
    match decoder.decode(packets) {
        Some(block) => {
            let output = unsafe { slice::from_raw_parts_mut(out, block.len()) };
            output.copy_from_slice(&block);
            true
        }
        None => false,
    }
}
