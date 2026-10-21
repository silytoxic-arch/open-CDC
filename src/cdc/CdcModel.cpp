#include "cdc/CdcModel.h"

namespace opencdc::cdc {

std::string_view toString(SourceKind v) noexcept {
    switch (v) {
        case SourceKind::Register: return "register";
        case SourceKind::Latch: return "latch";
        case SourceKind::PrimaryInput: return "input";
        case SourceKind::BlackBox: return "black-box";
    }
    return "?";
}

std::string_view toString(Pin v) noexcept {
    switch (v) {
        case Pin::Data: return "data";
        case Pin::Enable: return "enable";
        case Pin::SyncReset: return "sync-reset";
        case Pin::AsyncReset: return "async-reset";
    }
    return "?";
}

std::string_view toString(SyncType v) noexcept {
    switch (v) {
        case SyncType::None: return "none";
        case SyncType::SingleFlop: return "single-flop";
        case SyncType::TwoFlop: return "two-flop";
        case SyncType::ThreeFlop: return "three-flop";
        case SyncType::MultiFlop: return "multi-flop";
        case SyncType::QualifiedData: return "qualified-data";
        case SyncType::Toggle: return "toggle";
        case SyncType::Gray: return "gray";
        case SyncType::Handshake: return "handshake";
        case SyncType::AsyncFifo: return "async-fifo";
        case SyncType::ResetSync: return "reset-sync";
    }
    return "?";
}

std::string_view toString(SyncStatus v) noexcept {
    switch (v) {
        case SyncStatus::NotSynchronized: return "not-synchronized";
        case SyncStatus::StructureRecognized: return "structure-recognized";
        case SyncStatus::NecessaryConditionsVerified: return "necessary-conditions-verified";
        case SyncStatus::ProtocolUnverified: return "protocol-unverified";
    }
    return "?";
}

std::string_view toString(AnalysisStatus v) noexcept {
    switch (v) {
        case AnalysisStatus::Safe: return "safe";
        case AnalysisStatus::NeedsReview: return "needs-review";
        case AnalysisStatus::PotentialRisk: return "potential-risk";
        case AnalysisStatus::Violation: return "violation";
    }
    return "?";
}

std::string_view toString(CrossingClass v) noexcept {
    switch (v) {
        case CrossingClass::SingleBitControl: return "single-bit-control";
        case CrossingClass::SingleBitData: return "single-bit-data";
        case CrossingClass::MultiBitControl: return "multi-bit-control";
        case CrossingClass::MultiBitData: return "multi-bit-data";
    }
    return "?";
}

} // namespace opencdc::cdc
