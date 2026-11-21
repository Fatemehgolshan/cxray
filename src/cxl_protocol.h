#pragma once
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// CXL Protocol Structures
// M2S = Master-to-Slave (Host → Device)
// S2M = Slave-to-Master (Device → Host)
// ---------------------------------------------------------------------------

namespace cxl {

// ── Opcodes ─────────────────────────────────────────────────────────────────

enum class M2SOpcode : uint8_t {
    MemRd      = 0,  // Read cacheline
    MemWr      = 1,  // Write cacheline (full)
    MemRdData  = 2,  // Read + send data (for RMW)
    MemWrPtl   = 3,  // Partial write (dirty eviction)
    MemInv     = 4,  // Invalidate cacheline
};

enum class S2MOpcode : uint8_t {
    MemData    = 0,  // Data response to read
    MemRsp     = 1,  // Acknowledgment (no data)
    BIRsp      = 2,  // Back-Invalidation response
};

enum class SnpType : uint8_t {
    NoSnp         = 0,
    SnpAll        = 1,
    SnpNotShared  = 2,
};

// ── Transaction state machine ────────────────────────────────────────────────

enum class TxState : uint8_t {
    IDLE,
    QUEUED,
    LINK_TX,
    DEVICE_QUEUED,
    DRAM_PENDING,
    LINK_RX,
    DONE,
};

// ── Packet structures ────────────────────────────────────────────────────────

struct M2SReq {
    M2SOpcode  opcode      = M2SOpcode::MemRd;
    SnpType    snpType     = SnpType::NoSnp;
    uint64_t   addr        = 0;      // 64B-aligned physical address
    uint8_t    tag         = 0;      // transaction tag (for reorder)
    uint8_t    metaField   = 0;      // QoS / priority
    uint64_t   arrivalNs   = 0;      // arrival time at host controller (ns)
    uint32_t   requesterId = 0;
    bool       poison      = false;  // pre-poisoned flag (POISON trace flag)
};

struct S2MResp {
    S2MOpcode  opcode       = S2MOpcode::MemData;
    uint8_t    tag          = 0;
    uint8_t    data[64]     = {};    // 64B cacheline payload
    bool       poison       = false; // data poison bit
    uint64_t   completeNs   = 0;     // completion time
};

// ── FLIT descriptor — generation-aware ───────────────────────────────────────
// CXL FLITs differ by PCIe generation. We model the FLIT as a descriptor
// (not packed bits) since we are a latency/bandwidth simulator, not a
// bit-accurate emulator.
//
//   Gen 5 NRZ  (CXL 1.1/2.0): 528-bit FLIT = 66 B total, 64 B payload
//     - 4×16 B data slots, 2 B header, 2 B CRC-16
//   Gen 6 PAM4 (CXL 3.0/3.1): 2048-bit FLIT = 256 B total, ~236 B payload
//     - After RS-FEC parity (14 B) and header (6 B)
//

struct CXLFlit {
    // ── Physical constants ─────────────────────────────────────────────────
    static constexpr uint32_t GEN5_SIZE_BYTES    = 66;
    static constexpr uint32_t GEN5_PAYLOAD_BYTES = 64;
    static constexpr uint32_t GEN5_NUM_SLOTS     = 4;   // 4×16 B data slots

    static constexpr uint32_t GEN6_SIZE_BYTES    = 256;
    static constexpr uint32_t GEN6_PAYLOAD_BYTES = 236; // 256 - 14 (FEC) - 6 (hdr)
    static constexpr uint32_t GEN6_NUM_SLOTS     = 14;  // 14×16 B data slots + FEC region

    // ── Runtime descriptor (set at construction) ───────────────────────────
    uint32_t sizeBytes    = GEN5_SIZE_BYTES;
    uint32_t payloadBytes = GEN5_PAYLOAD_BYTES;
    int      pcieGen      = 5;

    // ── Factories ──────────────────────────────────────────────────────────
    static CXLFlit gen5() { return {GEN5_SIZE_BYTES, GEN5_PAYLOAD_BYTES, 5}; }
    static CXLFlit gen6() { return {GEN6_SIZE_BYTES, GEN6_PAYLOAD_BYTES, 6}; }
    static CXLFlit fromPCIeGen(int gen) { return gen >= 6 ? gen6() : gen5(); }

    // How many FLITs needed to carry `payloadBytes` bytes of user data?
    int numFlits(uint32_t userDataBytes) const {
        return static_cast<int>(
            (userDataBytes + payloadBytes - 1) / payloadBytes);
    }

    // ── Legacy compatibility ───────────────────────────────────────────────
    // Keep old SIZE_BYTES / PAYLOAD_BYTES / NUM_SLOTS as Gen5 defaults
    // so existing code that references them still compiles.
    static constexpr uint32_t SIZE_BYTES    = GEN5_SIZE_BYTES;
    static constexpr uint32_t PAYLOAD_BYTES = GEN5_PAYLOAD_BYTES;
    static constexpr uint32_t NUM_SLOTS     = GEN5_NUM_SLOTS;
};

// ── Request trace entry (from .trace file) ──────────────────────────────────

enum class TraceOp : uint8_t {
    READ   = 0,
    WRITE  = 1,
    RMW    = 2,
    FLUSH  = 3,
};

struct TraceRequest {
    uint64_t   timestampNs  = 0;
    TraceOp    op           = TraceOp::READ;
    uint64_t   addr         = 0;
    uint32_t   sizeBytes    = 64;
    uint32_t   requesterId  = 0;
    bool       flagPoison   = false;
    bool       flagNoSnp    = false;
    bool       flagPrefetch = false;
};

// ── Result per request ──────────────────────────────────────────────────────

struct RequestResult {
    uint64_t   timestampNs  = 0;
    uint64_t   completeNs   = 0;
    double     latencyNs    = 0.0;
    TraceOp    op           = TraceOp::READ;
    uint64_t   addr         = 0;
    uint32_t   requesterId  = 0;
    bool       rowHit       = false;
    double     creditStallNs = 0.0;
};

}  // namespace cxl
