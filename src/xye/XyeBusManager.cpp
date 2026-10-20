#include "fl/xye/XyeBusManager.h"

namespace fl {
namespace {
constexpr MonotonicMs kCommandRateLimitMs = 2000;
constexpr MonotonicMs kCommandVerifyDelayMs = 500;
constexpr std::uint8_t kCommandVerifyRetries = 2;
void log(EventLog& sink, Severity severity, const char* message) {
    LogEvent event; event.severity = severity; event.message.assign(message); sink.append(event);
}
template <typename T> void timestamp(Sample<T>& sample, MonotonicMs now) { sample.updated_ms = now; }
void timestampObserved(ObservedState& observed, MonotonicMs now) {
    timestamp(observed.power, now); timestamp(observed.mode, now);
    timestamp(observed.fan_selected, now); timestamp(observed.fan_actual, now); timestamp(observed.fan_auto, now);
    timestamp(observed.setpoint_deci_c, now); timestamp(observed.t1_deci_c, now);
    timestamp(observed.t2_deci_c, now); timestamp(observed.t2b_deci_c, now);
    timestamp(observed.t3_deci_c, now); timestamp(observed.compressor_running, now);
    timestamp(observed.drain_pump, now); timestamp(observed.water_lock, now);
    timestamp(observed.error_flags, now); timestamp(observed.protection_flags, now);
    timestamp(observed.ccm_error_flags, now); timestamp(observed.capabilities_raw, now);
}
void promoteExperimentalSamples(ObservedState& observed) {
    auto promote = [](auto& sample) {
        if (sample.status == ValueStatus::Experimental) sample.status = ValueStatus::Valid;
    };
    promote(observed.power); promote(observed.mode); promote(observed.fan_selected);
    promote(observed.fan_actual); promote(observed.fan_auto); promote(observed.setpoint_deci_c);
    promote(observed.t1_deci_c); promote(observed.t2_deci_c); promote(observed.t2b_deci_c);
    promote(observed.t3_deci_c); promote(observed.compressor_running); promote(observed.drain_pump);
    promote(observed.water_lock); promote(observed.error_flags); promote(observed.protection_flags);
    promote(observed.ccm_error_flags); promote(observed.capabilities_raw);
}
}

XyeBusManager::XyeBusManager(IRs485Transport& transport, IXyeCodec& codec, Config& config,
                             IConfigRepository& repository, EventLog& event_log)
    : transport_(transport), codec_(codec), config_(config), repository_(repository), log_(event_log) {}

Result XyeBusManager::begin() { return transport_.begin(); }
Result XyeBusManager::startScan() {
    if (operation_ != Operation::Idle || active_command_valid_ || queue_count_ != 0) return Result::Busy;
    operation_ = Operation::Scan; current_address_ = 0; retries_ = 0; awaiting_ = false;
    scan_reset_pending_ = true;
    return Result::Accepted;
}
Result XyeBusManager::saveRegistry(Registry& next) {
    if (next.size() > kMaxAc) return Result::Full;
    const Result result = repository_.saveRegistry(next);
    if (result != Result::Accepted) return result;
    config_.registry = next;
    return Result::Accepted;
}
Result XyeBusManager::registerAc(const AcRegistration& item) {
    if (!validAddress(item.address) || item.name.view().empty() ||
        item.min_setpoint_deci_c >= item.max_setpoint_deci_c) return Result::Invalid;
    Registry next = config_.registry;
    for (const auto& current : next) if (current.address == item.address) return Result::Busy;
    if (!next.push_back(item)) return Result::Full;
    return saveRegistry(next);
}
Result XyeBusManager::updateAc(const AcRegistration& item) {
    if (!validAddress(item.address) || item.name.view().empty() ||
        item.min_setpoint_deci_c >= item.max_setpoint_deci_c) return Result::Invalid;
    Registry next = config_.registry;
    for (auto& current : next) if (current.address == item.address) { current = item; return saveRegistry(next); }
    return Result::NotFound;
}
Result XyeBusManager::removeAc(Address address) {
    if ((active_command_valid_ && active_command_.address == address)) return Result::Busy;
    for (std::size_t i = 0; i < queue_count_; ++i)
        if (queue_[(queue_head_ + i) % queue_.size()].address == address) return Result::Busy;
    for (const auto& group : config_.groups)
        for (const auto member : group.members) if (member == address) return Result::Forbidden;
    Registry next;
    bool found = false;
    for (const auto& current : config_.registry) {
        if (current.address == address) { found = true; continue; }
        if (!next.push_back(current)) return Result::Full;
    }
    return found ? saveRegistry(next) : Result::NotFound;
}

Result XyeBusManager::enqueue(const ControlRequest& input) {
    if (!input.settings.power || !validAddress(input.address))
        return Result::Invalid;
    const AcRegistration* registered = nullptr;
    for (const auto& item : config_.registry) if (item.address == input.address) { registered = &item; break; }
    if (!registered) return Result::NotFound;
    if (!registered->model_validated) return Result::Forbidden;
    if (input.origin == CommandOrigin::Web) {
        bool auto_group_member = false;
        for (const auto& group : config_.groups)
            if (group.mode == GroupMode::Auto)
                for (const auto member : group.members) if (member == input.address) auto_group_member = true;
        if (auto_group_member && !registered->maintenance) return Result::Forbidden;
    } else if (input.origin == CommandOrigin::Rotation) {
        if (registered->maintenance || !config_.automatic_control_enabled || !input.group_id || !current_state_ || current_state_->observe_only)
            return Result::Forbidden;
        bool allowed = false;
        for (const auto& group : config_.groups) if (group.id == *input.group_id && group.mode == GroupMode::Auto)
            for (const auto member : group.members) if (member == input.address) allowed = true;
        if (!allowed) return Result::Forbidden;
    } else return Result::Forbidden;
    if (*input.settings.power) {
        // ON requires a selected group or an explicit manual operator request.
        if (input.origin == CommandOrigin::Rotation && !input.group_id) return Result::Forbidden;
    }
    if (!current_state_) return Result::Busy;
    const AcState* runtime_ac = nullptr;
    for (const auto& ac : current_state_->conditioners) if (ac.address == input.address) { runtime_ac = &ac; break; }
    if (!runtime_ac || runtime_ac->availability != Availability::Online || runtime_ac->freshness != Freshness::Fresh ||
        !runtime_ac->last_seen_ms || now_ms_ < *runtime_ac->last_seen_ms ||
        now_ms_ - *runtime_ac->last_seen_ms > 2 * config_.bus.poll_interval_ms) return Result::Forbidden;

    TxFrame validation{};
    if (codec_.encodeSet(input, validation) != Result::Accepted) return Result::Invalid;
    if (input.settings.power.value() && input.settings.mode && *input.settings.mode != Mode::Fan) {
        const auto value = input.settings.setpoint_deci_c;
        if (!value || *value < registered->min_setpoint_deci_c || *value > registered->max_setpoint_deci_c)
            return Result::Invalid;
    }
    if (active_command_valid_ && active_command_.address == input.address) return Result::Busy;
    for (std::size_t i = 0; i < queue_count_; ++i)
        if (queue_[(queue_head_ + i) % queue_.size()].address == input.address) return Result::Busy;
    if (queue_count_ == queue_.size()) return Result::Full;
    if (has_command_time_.test(input.address) && now_ms_ >= last_command_ms_[input.address] &&
        now_ms_ - last_command_ms_[input.address] < kCommandRateLimitMs) return Result::Busy;

    ControlRequest request = input;
    if (request.transaction_id == 0) {
        request.transaction_id = next_transaction_id_++;
        if (next_transaction_id_ == 0) next_transaction_id_ = 1;
    }
    queue_[(queue_head_ + queue_count_) % queue_.size()] = request;
    ++queue_count_;
    last_command_ms_[request.address] = now_ms_;
    has_command_time_.set(request.address);
    return Result::Accepted;
}

void XyeBusManager::applySuccess(RuntimeState& state, Address address,
                                 ObservedState observed, MonotonicMs now) {
    for (const auto& registration : config_.registry) if (registration.address == address) {
        if (registration.model_validated) promoteExperimentalSamples(observed);
        break;
    }
    for (auto& ac : state.conditioners) if (ac.address == address) {
        if (observed.power.value && *observed.power.value &&
            (!ac.observed.power.value || !*ac.observed.power.value || !ac.last_started_ms)) ac.last_started_ms = now;
        if (observed.power.value && !*observed.power.value &&
            ((ac.observed.power.value && *ac.observed.power.value) || !ac.last_stopped_ms)) ac.last_stopped_ms = now;
        ac.observed = observed; ac.last_seen_ms = now; ac.freshness = Freshness::Fresh;
        ac.consecutive_failures = 0;
        if (ac.consecutive_successes < 255) ++ac.consecutive_successes;
        if (ac.consecutive_successes >= config_.bus.online_successes) ac.availability = Availability::Online;
        return;
    }
}
void XyeBusManager::applyFailure(RuntimeState& state, Address address) {
    for (auto& ac : state.conditioners) if (ac.address == address) {
        ac.consecutive_successes = 0;
        if (ac.consecutive_failures < 255) ++ac.consecutive_failures;
        if (ac.consecutive_failures >= config_.bus.offline_failures) {
            ac.availability = Availability::Offline; ac.freshness = Freshness::Stale;
        }
        return;
    }
}
void XyeBusManager::resetAwaiting(MonotonicMs now) {
    awaiting_ = false; tx_complete_ = false; rx_length_ = 0; next_action_ms_ = now;
}
Result XyeBusManager::encodeCurrent(TxFrame& frame) const {
    if (operation_ == Operation::Command) return codec_.encodeSet(active_command_, frame);
    return codec_.encodeQuery(current_address_, frame);
}
void XyeBusManager::advanceScan(RuntimeState& state, MonotonicMs now, bool success) {
    if (success) state.bus.discovered.set(current_address_);
    ++state.bus.scan_completed;
    if (current_address_ == kAddressCount - 1) {
        operation_ = Operation::Idle; state.bus.scan_running = false;
        log(log_, Severity::Info, "Read-only XYE scan completed");
    } else ++current_address_;
    retries_ = 0; resetAwaiting(now);
}
void XyeBusManager::advancePoll(RuntimeState& state, MonotonicMs now, bool success) {
    if (success) ++state.bus.poll_ok;
    else { ++state.bus.timeouts; applyFailure(state, current_address_); }
    if (!config_.registry.size()) { operation_ = Operation::Idle; resetAwaiting(now); return; }
    if (++poll_index_ >= config_.registry.size()) {
        poll_index_ = 0; operation_ = Operation::Idle;
        next_cycle_ms_ = now + config_.bus.poll_interval_ms;
        commands_since_poll_ = 0;
    } else current_address_ = config_.registry[poll_index_].address;
    retries_ = 0; resetAwaiting(now);
}
void XyeBusManager::startCommand(RuntimeState& state, MonotonicMs now) {
    active_command_ = queue_[queue_head_];
    queue_head_ = (queue_head_ + 1) % queue_.size(); --queue_count_;
    active_command_valid_ = true; operation_ = Operation::Command; current_address_ = active_command_.address;
    retries_ = 0; command_retries_ = 0; verify_retries_ = 0; commands_since_poll_ = 1;
    for (auto& ac : state.conditioners) if (ac.address == current_address_) {
        // Commands may be partial (notably OFF). Keep the last explicitly chosen
        // mode, fan and setpoint so the UI can restore them after power-off.
        auto& desired = ac.desired.settings;
        if (active_command_.settings.power) desired.power = active_command_.settings.power;
        if (active_command_.settings.mode) desired.mode = active_command_.settings.mode;
        if (active_command_.settings.fan) desired.fan = active_command_.settings.fan;
        if (active_command_.settings.setpoint_deci_c)
            desired.setpoint_deci_c = active_command_.settings.setpoint_deci_c;
        ac.desired.command_state = CommandState::Pending;
        ac.desired.transaction_id = active_command_.transaction_id; ac.desired.origin = active_command_.origin;
        ac.desired.submitted_ms = now; ac.desired.failure = Result::Accepted; break;
    }
    log(log_, Severity::Info, active_command_.origin == CommandOrigin::Rotation ?
        "Automatic XYE C3 command started" : "Manual XYE C3 command started");
    resetAwaiting(now);
}
void XyeBusManager::finishCommand(RuntimeState& state, MonotonicMs now, Result result) {
    for (auto& ac : state.conditioners) if (ac.address == active_command_.address &&
        ac.desired.transaction_id == active_command_.transaction_id) {
        ac.desired.command_state = result == Result::Accepted ? CommandState::Acknowledged : CommandState::Failed;
        ac.desired.failure = result;
        break;
    }
    const bool automatic = active_command_.origin == CommandOrigin::Rotation;
    const char* message = result == Result::Accepted ?
        (automatic ? "Automatic XYE command verified by C0" : "Manual XYE command verified by C0") :
        (automatic ? "Automatic XYE command was not verified" : "Manual XYE command was not verified");
    log(log_, result == Result::Accepted ? Severity::Info : Severity::Warning, message);
    active_command_valid_ = false; operation_ = Operation::Idle;
    retries_ = 0; command_retries_ = 0; next_cycle_ms_ = now;
    resetAwaiting(now);
}
void XyeBusManager::scheduleVerifyRetry(MonotonicMs now) {
    ++verify_retries_;
    operation_ = Operation::Verify; retries_ = 0;
    resetAwaiting(now);
    next_action_ms_ = now + kCommandVerifyDelayMs;
}
bool XyeBusManager::matchesDesired(const ObservedState& observed) const {
    if (!active_command_.settings.power || !observed.power.value ||
        *observed.power.value != *active_command_.settings.power) return false;
    if (!*active_command_.settings.power) return true;
    if (!active_command_.settings.mode || !observed.mode.value ||
        *observed.mode.value != *active_command_.settings.mode) return false;
    if (*active_command_.settings.mode != Mode::Fan) {
        if (!active_command_.settings.setpoint_deci_c || !observed.setpoint_deci_c.value ||
            *observed.setpoint_deci_c.value != *active_command_.settings.setpoint_deci_c) return false;
    }
    // C0 exposes actual fan speed, not the selected fan setting. Keep fan
    // selection as a desired value until a model-specific setting read is added.
    return true;
}

void XyeBusManager::tick(const Config&, RuntimeState& state, MonotonicMs now) {
    current_state_ = &state; now_ms_ = now;
    for (std::size_t i = 0; i < state.conditioners.size();) {
        bool registered = false;
        for (const auto& item : config_.registry) if (item.address == state.conditioners[i].address) {
            registered = true; break;
        }
        if (!registered) state.conditioners.erase(i); else ++i;
    }
    for (const auto& registered : config_.registry) {
        bool present = false;
        for (auto& current : state.conditioners) if (current.address == registered.address) {
            present = true; current.model_validated = registered.model_validated;
            current.maintenance = registered.maintenance; current.name = registered.name; break;
        }
        if (!present && state.conditioners.size() < kMaxAc) {
            AcState ac; ac.address = registered.address; ac.name = registered.name;
            ac.maintenance = registered.maintenance; ac.model_validated = registered.model_validated;
            state.conditioners.push_back(ac);
        }
    }
    // The decoder labels model-dependent C0 fields experimental. Once the owner
    // has compared this unit's live values with its panel, promote that model's
    // fields so the safety engine can distinguish vetted readings from guesses.
    for (auto& ac : state.conditioners) if (ac.model_validated)
        promoteExperimentalSamples(ac.observed);
    if (operation_ == Operation::Scan) {
        state.bus.scan_running = true;
        if (scan_reset_pending_) {
            state.bus.scan_completed = 0; state.bus.discovered.reset(); scan_reset_pending_ = false;
        }
    }

    if (awaiting_) {
        std::uint8_t incoming[32];
        const auto count = transport_.read(incoming, sizeof(incoming));
        for (std::size_t i = 0; i < count; ++i) {
            if (rx_length_ == 0 && incoming[i] != 0xAA) { ++state.bus.rejected_frames; continue; }
            if (rx_length_ < rx_.size()) rx_[rx_length_++] = incoming[i];
            if (rx_length_ == rx_.size()) {
                const auto expected = operation_ == Operation::Command ? XyeCommand::Set : XyeCommand::Query;
                ObservedState decoded;
                const auto result = codec_.decode(rx_, current_address_, expected, decoded);
                if (result == Result::Accepted) {
                    timestampObserved(decoded, now);
                    if (operation_ == Operation::Scan) { advanceScan(state, now, true); return; }
                    if (operation_ == Operation::Poll) {
                        applySuccess(state, current_address_, decoded, now); advancePoll(state, now, true); return;
                    }
                    if (operation_ == Operation::Command) {
                        // C3 acknowledgement proves a response, while the following
                        // C0 transaction verifies the state the unit actually applies.
                        operation_ = Operation::Verify; retries_ = 0; verify_retries_ = 0;
                        resetAwaiting(now); next_action_ms_ = now + kCommandVerifyDelayMs; return;
                    }
                    if (operation_ == Operation::Verify) {
                        applySuccess(state, current_address_, decoded, now);
                        if (matchesDesired(decoded)) finishCommand(state, now, Result::Accepted);
                        else if (verify_retries_ < kCommandVerifyRetries) scheduleVerifyRetry(now);
                        else finishCommand(state, now, Result::IoError);
                        return;
                    }
                }
                if (!validXyeChecksum(rx_)) ++state.bus.checksum_errors;
                ++state.bus.rejected_frames; rx_length_ = 0;
            }
        }
        if (!tx_complete_ && transport_.txIdle()) {
            tx_complete_ = true; deadline_ = now + config_.bus.response_timeout_ms;
        }
        if (tx_complete_ && now >= deadline_) {
            if (operation_ == Operation::Verify) {
                if (verify_retries_ < kCommandVerifyRetries) scheduleVerifyRetry(now);
                else finishCommand(state, now, Result::IoError);
            } else if (operation_ == Operation::Command) {
                if (command_retries_ < config_.bus.retry_limit) {
                    ++command_retries_; operation_ = Operation::Command; retries_ = 0; resetAwaiting(now);
                    TxFrame retry{};
                    if (encodeCurrent(retry) == Result::Accepted &&
                        transport_.write(retry.data(), retry.size()) == retry.size()) {
                        awaiting_ = true; tx_complete_ = false;
                    } else { ++state.bus.rejected_frames; finishCommand(state, now, Result::IoError); }
                } else finishCommand(state, now, Result::IoError);
            } else if (retries_ < config_.bus.retry_limit) {
                TxFrame retry{};
                if (encodeCurrent(retry) == Result::Accepted &&
                    transport_.write(retry.data(), retry.size()) == retry.size()) {
                    ++retries_; tx_complete_ = false; rx_length_ = 0;
                } else {
                    ++state.bus.rejected_frames;
                    if (operation_ == Operation::Scan) advanceScan(state, now, false);
                    else advancePoll(state, now, false);
                }
            } else if (operation_ == Operation::Scan) advanceScan(state, now, false);
            else advancePoll(state, now, false);
        }
        return;
    }

    if (operation_ == Operation::Idle) {
        if (queue_count_ && commands_since_poll_ == 0) startCommand(state, now);
        else if (config_.registry.size() && now >= next_cycle_ms_) {
            operation_ = Operation::Poll;
            if (poll_index_ >= config_.registry.size()) poll_index_ = 0;
            current_address_ = config_.registry[poll_index_].address;
        } else return;
    } else if (operation_ == Operation::Poll && config_.registry.size()) {
        if (poll_index_ >= config_.registry.size()) poll_index_ = 0;
        current_address_ = config_.registry[poll_index_].address;
    }
    if (now < next_action_ms_ || !transport_.txIdle()) return;
    TxFrame frame{};
    if (encodeCurrent(frame) != Result::Accepted ||
        transport_.write(frame.data(), frame.size()) != frame.size()) {
        ++state.bus.rejected_frames;
        if (operation_ == Operation::Scan) advanceScan(state, now, false);
        else if (operation_ == Operation::Poll) advancePoll(state, now, false);
        else finishCommand(state, now, Result::IoError);
        return;
    }
    awaiting_ = true; tx_complete_ = false; rx_length_ = 0;
}
} // namespace fl
