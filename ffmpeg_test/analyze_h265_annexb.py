#!/usr/bin/env python3
import argparse
from collections import Counter

NAL_TYPE_NAMES = {
    0: "TRAIL_N", 1: "TRAIL_R",
    2: "TSA_N", 3: "TSA_R",
    4: "STSA_N", 5: "STSA_R",
    6: "RADL_N", 7: "RADL_R",
    8: "RASL_N", 9: "RASL_R",
    16: "BLA_W_LP", 17: "BLA_W_RADL", 18: "BLA_N_LP",
    19: "IDR_W_RADL", 20: "IDR_N_LP", 21: "CRA_NUT",
    32: "VPS", 33: "SPS", 34: "PPS", 35: "AUD", 39: "SEI_PREFIX", 40: "SEI_SUFFIX"
}


def find_start_codes(data: bytes):
    """Return list of (offset, start_code_len). Support 3-byte and 4-byte start codes."""
    positions = []
    i, n = 0, len(data)
    while i < n - 3:
        if data[i] == 0 and data[i+1] == 0:
            if data[i+2] == 1:
                positions.append((i, 3))
                i += 3
                continue
            if i + 3 < n and data[i+2] == 0 and data[i+3] == 1:
                positions.append((i, 4))
                i += 4
                continue
        i += 1
    return positions


def parse_annexb_nalus(data: bytes):
    starts = find_start_codes(data)
    nalus = []
    for idx, (off, sc_len) in enumerate(starts):
        payload_start = off + sc_len
        payload_end = starts[idx + 1][0] if idx + 1 < len(starts) else len(data)
        if payload_start >= payload_end:
            continue

        nalu = data[payload_start:payload_end]
        if len(nalu) < 2:
            continue

        # HEVC NALU header is 2 bytes
        # forbidden_zero_bit: 1 bit
        # nal_unit_type: 6 bits
        # nuh_layer_id: 6 bits
        # nuh_temporal_id_plus1: 3 bits
        first = nalu[0]
        nal_type = (first & 0x7E) >> 1

        nalus.append({
            "index": len(nalus),
            "offset": off,
            "start_code_len": sc_len,
            "payload_offset": payload_start,
            "payload_size": len(nalu),
            "nal_type": nal_type,
            "nal_name": NAL_TYPE_NAMES.get(nal_type, f"TYPE_{nal_type}"),
            "first_bytes": nalu[:8].hex(" "),
        })
    return nalus


def main():
    ap = argparse.ArgumentParser(description="Analyze Annex-B HEVC (.h265/.hevc) stream")
    ap.add_argument("input", help="input h265 file")
    ap.add_argument("--show", type=int, default=30, help="show first N NALUs (default: 30)")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = f.read()

    nalus = parse_annexb_nalus(data)
    if not nalus:
        print("No Annex-B start codes/NALUs found.")
        return

    print("=== Annex-B HEVC 统计过程 ===")
    print(f"文件: {args.input}")
    print(f"总大小: {len(data)} bytes")
    print(f"检测到 NALU 数量: {len(nalus)}")
    print()

    print("[步骤1] 列出前几个 NALU（偏移/类型/长度）")
    print("idx | start_off | sc | payload_off | payload_size | type(name) | first_bytes")
    for n in nalus[:args.show]:
        print(
            f"{n['index']:>3} | 0x{n['offset']:08x} | {n['start_code_len']}  | "
            f"0x{n['payload_offset']:08x} | {n['payload_size']:>11} | "
            f"{n['nal_type']:>2}({n['nal_name']}) | {n['first_bytes']}"
        )
    print()

    print("[步骤2] 统计各 NAL 类型数量")
    c = Counter(n["nal_type"] for n in nalus)
    for t, cnt in sorted(c.items(), key=lambda x: (x[0])):
        print(f"type {t:>2} ({NAL_TYPE_NAMES.get(t, f'TYPE_{t}')}): {cnt}")
    print()

    vps = c.get(32, 0)
    sps = c.get(33, 0)
    pps = c.get(34, 0)
    idr = c.get(19, 0) + c.get(20, 0)
    cra = c.get(21, 0)

    print("[步骤3] 参数集与关键帧接入点")
    print(f"VPS(type32): {vps}")
    print(f"SPS(type33): {sps}")
    print(f"PPS(type34): {pps}")
    print(f"IDR(type19+20): {idr}")
    print(f"CRA(type21): {cra}")
    print()

    if vps and sps and pps:
        print("结论: 流中存在 VPS/SPS/PPS，且可用于中途入流解码。")
    else:
        print("结论: 参数集不完整，可能影响解码器启动。")

    # Rough GOP observation: locate access points (IDR/CRA) by NAL order
    access_points = [n for n in nalus if n["nal_type"] in (19, 20, 21)]
    if access_points:
        print("\n[步骤4] 关键接入点（前20个）")
        for n in access_points[:20]:
            print(f"AP idx={n['index']} off=0x{n['offset']:08x} type={n['nal_type']}({n['nal_name']})")


if __name__ == "__main__":
    main()
