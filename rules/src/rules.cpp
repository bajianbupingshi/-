#include "gw/rules.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace gw {
namespace {

using json = nlohmann::json;

bool compare(std::uint16_t value, ThresholdRule::Op op, std::uint16_t threshold) {
    switch (op) {
        case ThresholdRule::Op::Gt: return value > threshold;
        case ThresholdRule::Op::Lt: return value < threshold;
        case ThresholdRule::Op::Ge: return value >= threshold;
        case ThresholdRule::Op::Le: return value <= threshold;
        case ThresholdRule::Op::Eq: return value == threshold;
        case ThresholdRule::Op::Ne: return value != threshold;
    }
    return false;
}

// 解除判定：越限后须回落出迟滞带（用带符号数学避免 u16 减法下溢）
bool cleared(std::uint16_t value, ThresholdRule::Op op, std::uint16_t threshold,
             std::uint16_t hysteresis) {
    const int v = value;
    const int t = threshold;
    const int h = hysteresis;
    switch (op) {
        case ThresholdRule::Op::Gt: return v <= t - h;
        case ThresholdRule::Op::Lt: return v >= t + h;
        case ThresholdRule::Op::Ge: return v < t - h;
        case ThresholdRule::Op::Le: return v > t + h;
        case ThresholdRule::Op::Eq: return v != t + h && v != t - h;   // 简化：偏离 ±迟滞
        case ThresholdRule::Op::Ne: return v == t;
    }
    return false;
}

const char* op_name(ThresholdRule::Op op) {
    switch (op) {
        case ThresholdRule::Op::Gt: return "gt";
        case ThresholdRule::Op::Lt: return "lt";
        case ThresholdRule::Op::Ge: return "ge";
        case ThresholdRule::Op::Le: return "le";
        case ThresholdRule::Op::Eq: return "eq";
        case ThresholdRule::Op::Ne: return "ne";
    }
    return "?";
}

ThresholdRule::Op parse_op(const std::string& s) {
    if (s == "gt") return ThresholdRule::Op::Gt;
    if (s == "lt") return ThresholdRule::Op::Lt;
    if (s == "ge") return ThresholdRule::Op::Ge;
    if (s == "le") return ThresholdRule::Op::Le;
    if (s == "eq") return ThresholdRule::Op::Eq;
    if (s == "ne") return ThresholdRule::Op::Ne;
    throw std::runtime_error("rules json: 非法 op \"" + s + "\"（允许 gt/lt/ge/le/eq/ne）");
}

const json& get_field(const json& j, const char* key, const char* ctx) {
    if (!j.contains(key)) {
        throw std::runtime_error(std::string("rules json: 缺字段 \"") + key + "\"（" + ctx + "）");
    }
    return j.at(key);
}

std::string require_string(const json& j, const char* key, const char* ctx) {
    const json& v = get_field(j, key, ctx);
    if (!v.is_string()) {
        throw std::runtime_error(std::string("rules json: \"") + key + "\" 须为字符串（" + ctx + "）");
    }
    return v.get<std::string>();
}

ThresholdRule parse_threshold(const json& j) {
    ThresholdRule r;
    r.name = require_string(j, "name", "threshold");
    if (r.name.empty()) {
        throw std::runtime_error("rules json: name 不能为空");
    }
    r.addr = get_field(j, "addr", r.name.c_str()).get<std::uint16_t>();
    r.op = parse_op(require_string(j, "op", r.name.c_str()));
    r.threshold = get_field(j, "value", r.name.c_str()).get<std::uint16_t>();
    if (j.contains("hysteresis")) {
        r.hysteresis = j.at("hysteresis").get<std::uint16_t>();
    }
    if (j.contains("cooldown_ms")) {
        r.cooldown_ms = j.at("cooldown_ms").get<std::uint64_t>();
    }
    return r;
}

RateRule parse_rate(const json& j) {
    RateRule r;
    r.name = require_string(j, "name", "rate");
    if (r.name.empty()) {
        throw std::runtime_error("rules json: name 不能为空");
    }
    r.addr = get_field(j, "addr", r.name.c_str()).get<std::uint16_t>();
    r.max_delta_per_sec = get_field(j, "max_delta_per_sec", r.name.c_str()).get<double>();
    if (!(r.max_delta_per_sec > 0.0)) {
        throw std::runtime_error("rules json: max_delta_per_sec 须 > 0（" + r.name + "）");
    }
    if (j.contains("cooldown_ms")) {
        r.cooldown_ms = j.at("cooldown_ms").get<std::uint64_t>();
    }
    return r;
}

}  // namespace

RuleEngine RuleEngine::from_json(const std::string& json_text) {
    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("rules json: 语法错误 —— ") + e.what());
    }

    const json* rules = nullptr;
    if (root.is_array()) {
        rules = &root;
    } else if (root.is_object() && root.contains("rules") && root["rules"].is_array()) {
        rules = &root["rules"];
    } else {
        throw std::runtime_error("rules json: 须为 {\"rules\":[...]} 或裸数组");
    }

    RuleEngine engine;
    for (const json& item : *rules) {
        if (!item.is_object()) {
            throw std::runtime_error("rules json: 规则项须为对象");
        }
        const std::string type = require_string(item, "type", "rule");
        if (type == "threshold") {
            engine.thresholds_.push_back(parse_threshold(item));
        } else if (type == "rate") {
            engine.rates_.push_back(parse_rate(item));
        } else {
            throw std::runtime_error("rules json: 未知规则类型 \"" + type +
                                     "\"（允许 threshold / rate）");
        }
    }
    return engine;
}

std::vector<Alert> RuleEngine::evaluate(const std::vector<Point>& points, std::uint64_t ts_ms) {
    std::vector<Alert> alerts;

    for (const Point& p : points) {
        for (ThresholdRule& r : thresholds_) {
            if (r.addr != p.addr) {
                continue;
            }
            if (!r.has_seen) {
                r.has_seen = true;   // 首样本只建基线
                r.in_alarm = compare(p.value, r.op, r.threshold);
                continue;
            }
            if (!r.in_alarm) {
                if (compare(p.value, r.op, r.threshold)) {
                    r.in_alarm = true;
                    if (!r.has_alerted || ts_ms - r.last_alert_ms >= r.cooldown_ms) {
                        r.has_alerted = true;
                        r.last_alert_ms = ts_ms;
                        alerts.push_back(Alert{r.name, "threshold", p.addr, p.value, ts_ms,
                                               std::string("value=") + std::to_string(p.value) +
                                                   " " + op_name(r.op) + " " +
                                                   std::to_string(r.threshold)});
                    }
                }
            } else if (cleared(p.value, r.op, r.threshold, r.hysteresis)) {
                r.in_alarm = false;   // 回落出迟滞带：解除（不产告警，下此越限重新告）
            }
        }

        for (RateRule& r : rates_) {
            if (r.addr != p.addr) {
                continue;
            }
            if (!r.has_prev) {
                r.has_prev = true;   // 首样本建基线：没有 Δt 就没有变化率
                r.prev_value = p.value;
                r.prev_ts = ts_ms;
                continue;
            }
            const std::uint64_t dt = (ts_ms > r.prev_ts) ? ts_ms - r.prev_ts : 0;
            const int delta = std::abs(static_cast<int>(p.value) - static_cast<int>(r.prev_value));
            const double rate = (dt > 0) ? static_cast<double>(delta) * 1000.0 / static_cast<double>(dt)
                                         : 0.0;
            r.prev_value = p.value;
            r.prev_ts = ts_ms;
            if (dt > 0 && rate > r.max_delta_per_sec &&
                (!r.has_alerted || ts_ms - r.last_alert_ms >= r.cooldown_ms)) {
                r.has_alerted = true;
                r.last_alert_ms = ts_ms;
                alerts.push_back(Alert{r.name, "rate", p.addr, p.value, ts_ms,
                                       "delta=" + std::to_string(delta) + "/" +
                                           std::to_string(dt) + "ms = " +
                                           std::to_string(static_cast<long long>(rate)) +
                                           "/s > " +
                                           std::to_string(static_cast<long long>(r.max_delta_per_sec)) +
                                           "/s"});
            }
        }
    }
    return alerts;
}

}  // namespace gw
