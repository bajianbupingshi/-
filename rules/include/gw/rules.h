#ifndef GW_RULES_H
#define GW_RULES_H

#include <cstdint>
#include <string>
#include <vector>

#include "gw/reliability.h"

namespace gw {

// ─────────────────────────────────────────────────────────────────────────────
// 规则引擎（W3-3）：消费数据流 → 产出告警
//
// 两类规则（方案 §5③ 的「阈值 / 变化率」）：
//   · ThresholdRule：越限告警，带**迟滞**（防抖动：越限后须回落出迟滞带才算解除）
//     与**冷却**（两次告警的最小间隔，防刷屏）—— 真实告警系统的标配，不是装饰。
//   · RateRule：变化率告警 —— |Δvalue|/Δt 超过上限即触发（阶跃/突变检测）。
//
// 求值语义：
//   · evaluate(points, ts_ms) 纯粹由「规则状态 + 输入」决定输出 —— 无随机、无
//     环境依赖，同一条回放流喂两个新引擎得到逐字段相同的告警序列（确定性重放）。
//   · 时间由调用方给（管道线程传处理时刻）—— 引擎自己不看钟，时序可注入。
//
// 配置从 JSON 加载（方案 §4 选型 nlohmann/json），解析失败抛 std::runtime_error
// 并带字段级原因；nlohmann 头被挡在 .cpp 里，本头文件零第三方依赖。
// ─────────────────────────────────────────────────────────────────────────────

struct ThresholdRule {
    enum class Op { Gt, Lt, Ge, Le, Eq, Ne };

    std::string name;
    std::uint16_t addr = 0;
    Op op = Op::Gt;
    std::uint16_t threshold = 0;
    std::uint16_t hysteresis = 0;   // 迟滞带（解除阈值 = 阈值 ∓ 迟滞）
    std::uint64_t cooldown_ms = 0;  // 两次告警最小间隔

    // 运行期状态（evaluate 推进）
    bool has_seen = false;          // 是否见过首个样本（首样本只建基线不告警）
    bool in_alarm = false;
    bool has_alerted = false;
    std::uint64_t last_alert_ms = 0;
};

struct RateRule {
    std::string name;
    std::uint16_t addr = 0;
    double max_delta_per_sec = 0.0; // 上限（单位/秒），超过即告警
    std::uint64_t cooldown_ms = 0;

    // 运行期状态
    bool has_prev = false;
    std::uint16_t prev_value = 0;
    std::uint64_t prev_ts = 0;
    bool has_alerted = false;
    std::uint64_t last_alert_ms = 0;
};

struct Alert {
    std::string rule_name;
    std::string kind;       // "threshold" | "rate"
    std::uint16_t addr = 0;
    std::uint16_t value = 0;
    std::uint64_t ts_ms = 0;
    std::string detail;     // 人读细节（确定性格式化，重放逐字段一致）
};

class RuleEngine {
public:
    RuleEngine() = default;

    // 从 JSON 加载规则。接受 {"rules":[...]} 或裸数组 [...]；
    // 未知 type / 非法 op / 缺必填字段 ⇒ 抛 std::runtime_error（字段级原因）。
    // 未知键忽略（向前兼容）。
    static RuleEngine from_json(const std::string& json_text);

    // 消费一个点位批，返回本批触发的告警（顺序 = 规则定义顺序 × 点位顺序）。
    // 推进各规则的运行期状态 —— 非_const：状态机就在这里。
    std::vector<Alert> evaluate(const std::vector<Point>& points, std::uint64_t ts_ms);

    std::size_t rule_count() const noexcept { return thresholds_.size() + rates_.size(); }
    const std::vector<ThresholdRule>& threshold_rules() const noexcept { return thresholds_; }
    const std::vector<RateRule>& rate_rules() const noexcept { return rates_; }

private:
    std::vector<ThresholdRule> thresholds_;
    std::vector<RateRule> rates_;
};

}  // namespace gw

#endif  // GW_RULES_H
