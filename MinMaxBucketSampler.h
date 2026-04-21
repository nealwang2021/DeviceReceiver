#ifndef MINMAXBUCKETSAMPLER_H
#define MINMAXBUCKETSAMPLER_H

#include <QVector>
#include <algorithm>
#include <cmath>

/**
 * 像素桶 min/max 降采样。
 *
 * 思路：把给定区间按目标点数 N 划分为 B = ceil(N/2) 个桶；
 * 每个桶里保留极小、极大两个样本（保持时间顺序）。
 * 这样既能把总点数压到 ~N，又能保留桶内尖峰/低谷，避免等距采样丢尖峰。
 */
namespace MinMaxBucketSampler {

struct Result {
    QVector<double> keys;   // 输出时间轴（ms）
    QVector<double> values; // 输出数据值
};

/**
 * @param keysIn  输入时间轴（升序；允许为空）
 * @param valsIn  输入数据值（与 keysIn 等长）
 * @param startMs 仅保留 [startMs, endMs] 内的采样；若 startMs==endMs 则全量
 * @param endMs   同上
 * @param targetPoints 目标总点数（至少 2）；实际输出可能略少于该值。
 */
inline Result sample(const QVector<double>& keysIn,
                     const QVector<double>& valsIn,
                     double startMs,
                     double endMs,
                     int targetPoints)
{
    Result out;
    const int n = std::min(keysIn.size(), valsIn.size());
    if (n <= 0) {
        return out;
    }
    const bool useAllRange = !(endMs > startMs);

    // 定位区间 [iStart, iEnd)
    int iStart = 0;
    int iEnd = n;
    if (!useAllRange) {
        // 二分查找左边界
        auto lb = std::lower_bound(keysIn.begin(), keysIn.begin() + n, startMs);
        iStart = static_cast<int>(lb - keysIn.begin());
        auto ub = std::upper_bound(keysIn.begin(), keysIn.begin() + n, endMs);
        iEnd = static_cast<int>(ub - keysIn.begin());
    }
    if (iEnd - iStart <= 0) {
        return out;
    }

    const int count = iEnd - iStart;
    if (targetPoints < 4) {
        targetPoints = 4;
    }

    // 小数据直接返回（不需要桶化）
    if (count <= targetPoints) {
        out.keys.reserve(count);
        out.values.reserve(count);
        for (int i = iStart; i < iEnd; ++i) {
            const double v = valsIn.at(i);
            if (!std::isfinite(v)) continue;
            out.keys.append(keysIn.at(i));
            out.values.append(v);
        }
        return out;
    }

    const int buckets = std::max(2, targetPoints / 2);
    out.keys.reserve(buckets * 2);
    out.values.reserve(buckets * 2);

    const double totalSpan = static_cast<double>(count);
    for (int b = 0; b < buckets; ++b) {
        const int segStart = iStart + static_cast<int>(std::floor(static_cast<double>(b) * totalSpan / buckets));
        const int segEnd = iStart + static_cast<int>(std::floor(static_cast<double>(b + 1) * totalSpan / buckets));
        if (segEnd <= segStart) continue;

        int minIdx = -1, maxIdx = -1;
        double minVal = 0.0, maxVal = 0.0;
        for (int i = segStart; i < segEnd; ++i) {
            const double v = valsIn.at(i);
            if (!std::isfinite(v)) continue;
            if (minIdx < 0) {
                minIdx = maxIdx = i;
                minVal = maxVal = v;
            } else {
                if (v < minVal) { minVal = v; minIdx = i; }
                if (v > maxVal) { maxVal = v; maxIdx = i; }
            }
        }
        if (minIdx < 0) continue;
        if (minIdx == maxIdx) {
            out.keys.append(keysIn.at(minIdx));
            out.values.append(minVal);
        } else if (minIdx < maxIdx) {
            out.keys.append(keysIn.at(minIdx));
            out.values.append(minVal);
            out.keys.append(keysIn.at(maxIdx));
            out.values.append(maxVal);
        } else {
            out.keys.append(keysIn.at(maxIdx));
            out.values.append(maxVal);
            out.keys.append(keysIn.at(minIdx));
            out.values.append(minVal);
        }
    }
    return out;
}

} // namespace MinMaxBucketSampler

#endif // MINMAXBUCKETSAMPLER_H
