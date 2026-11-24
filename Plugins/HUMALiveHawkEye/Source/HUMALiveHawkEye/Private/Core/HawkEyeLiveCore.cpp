#include "HawkEyeLiveCore.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace hawkeye_live {

#include "FixedJointPose.inc"

const int kCoco29ToBody29[29] = {0, 1, 2, 3, 4, 27, 28, 5, 6, 7, 25, 26, 8, 9, 10, 11, 12, 13, 14,
                                 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};

// ---------------------------------------------------------------- snappy (raw)
bool SnappyDecompressRaw(const uint8_t* p, size_t n, std::vector<uint8_t>& out, std::string& error) {
  size_t i = 0;
  uint64_t len = 0;
  for (int shift = 0; shift < 35; shift += 7) {
    if (i >= n) { error = "snappy: truncated length"; return false; }
    uint8_t b = p[i++];
    len |= uint64_t(b & 0x7f) << shift;
    if (!(b & 0x80)) break;
  }
  if (len > (64u << 20)) { error = "snappy: too large"; return false; }
  out.clear();
  out.reserve(size_t(len));
  while (i < n) {
    const uint8_t tag = p[i++];
    const int type = tag & 3;
    if (type == 0) {  // literal
      size_t l = tag >> 2;
      if (l >= 60) {
        const int bytes = int(l) - 59;
        if (i + bytes > n) { error = "snappy: truncated literal length"; return false; }
        l = 0;
        for (int k = 0; k < bytes; ++k) l |= size_t(p[i + k]) << (8 * k);
        i += bytes;
      }
      l += 1;
      if (i + l > n) { error = "snappy: truncated literal"; return false; }
      out.insert(out.end(), p + i, p + i + l);
      i += l;
    } else {
      size_t l, off;
      if (type == 1) {
        if (i + 1 > n) { error = "snappy: truncated copy1"; return false; }
        l = ((tag >> 2) & 7) + 4;
        off = (size_t(tag >> 5) << 8) | p[i];
        i += 1;
      } else if (type == 2) {
        if (i + 2 > n) { error = "snappy: truncated copy2"; return false; }
        l = (tag >> 2) + 1;
        off = size_t(p[i]) | (size_t(p[i + 1]) << 8);
        i += 2;
      } else {
        if (i + 4 > n) { error = "snappy: truncated copy4"; return false; }
        l = (tag >> 2) + 1;
        off = size_t(p[i]) | (size_t(p[i + 1]) << 8) | (size_t(p[i + 2]) << 16) | (size_t(p[i + 3]) << 24);
        i += 4;
      }
      if (off == 0 || off > out.size()) { error = "snappy: bad offset"; return false; }
      const size_t start = out.size() - off;
      for (size_t k = 0; k < l; ++k) out.push_back(out[start + k]);  // 重なりあり
    }
  }
  if (out.size() != len) { error = "snappy: length mismatch"; return false; }
  return true;
}

// ---------------------------------------------------------------- BSON（必要な型だけ）
namespace {
int32_t rd_i32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
int64_t rd_i64(const uint8_t* p) { int64_t v; std::memcpy(&v, p, 8); return v; }
double rd_f64(const uint8_t* p) { double v; std::memcpy(&v, p, 8); return v; }

struct Value {
  uint8_t type = 0;
  const uint8_t* data = nullptr;  // 値の先頭
  size_t size = 0;                // 値のバイト数（文書・配列は全体、binary は中身）
};
// 文書 [p, p+n) の要素を順に返す。失敗時 false。
bool ForEach(const uint8_t* p, size_t n, const std::function<bool(const char*, const Value&)>& fn, std::string& err) {
  if (n < 5) { err = "bson: short document"; return false; }
  const int32_t total = rd_i32(p);
  if (total < 5 || size_t(total) > n) { err = "bson: bad document size"; return false; }
  size_t i = 4;
  const size_t end = size_t(total) - 1;
  while (i < end) {
    Value v;
    v.type = p[i++];
    const char* name = reinterpret_cast<const char*>(p + i);
    const void* z = std::memchr(p + i, 0, end - i);
    if (!z) { err = "bson: bad key"; return false; }
    i = size_t(static_cast<const uint8_t*>(z) - p) + 1;
    const size_t rest = end - i;
    switch (v.type) {
      case 0x01: v.size = 8; break;                                   // double
      case 0x02: if (rest < 4) { err = "bson: str"; return false; }  // string
        v.size = 4 + size_t(rd_i32(p + i)); break;
      case 0x03: case 0x04: if (rest < 4) { err = "bson: doc"; return false; }
        v.size = size_t(rd_i32(p + i)); break;
      case 0x05: if (rest < 5) { err = "bson: bin"; return false; }
        v.size = 5 + size_t(rd_i32(p + i)); break;
      case 0x08: v.size = 1; break;
      case 0x09: case 0x11: case 0x12: v.size = 8; break;
      case 0x0A: v.size = 0; break;
      case 0x10: v.size = 4; break;
      case 0x07: v.size = 12; break;
      default: err = "bson: unsupported type " + std::to_string(v.type); return false;
    }
    if (v.size > rest) { err = "bson: element overruns document"; return false; }
    v.data = p + i;
    if (!fn(name, v)) return false;
    i += v.size;
  }
  return true;
}
bool AsNumber(const Value& v, double& out) {
  switch (v.type) {
    case 0x01: out = rd_f64(v.data); return true;
    case 0x10: out = rd_i32(v.data); return true;
    case 0x12: case 0x09: case 0x11: out = double(rd_i64(v.data)); return true;
    case 0x02: out = std::atof(reinterpret_cast<const char*>(v.data + 4)); return true;
    default: return false;
  }
}
bool AsBinary(const Value& v, const uint8_t*& data, size_t& size) {
  if (v.type != 0x05) return false;
  data = v.data + 5;
  size = size_t(rd_i32(v.data));
  return true;
}
}  // namespace

bool ParseFrame(const uint8_t* data, size_t size, Frame& out, std::string& error) {
  std::vector<uint8_t> buffer;
  const uint8_t* doc = data;
  size_t doc_size = size;
  const bool plain = size >= 5 && size_t(rd_i32(data)) == size && data[size - 1] == 0;
  if (!plain) {
    if (!SnappyDecompressRaw(data, size, buffer, error)) return false;
    doc = buffer.data();
    doc_size = buffer.size();
  }
  out = Frame{};
  bool have_time = false;
  auto person = [&](const uint8_t* p, size_t n) -> bool {
    Person who;
    const uint8_t *ids = nullptr, *pos = nullptr, *com = nullptr;
    size_t nid = 0, npos = 0, ncom = 0;
    bool ok = ForEach(p, n, [&](const char* k, const Value& v) {
      double x;
      if (!std::strcmp(k, "trackId") && AsNumber(v, x)) who.track_id = int(x);
      else if (!std::strcmp(k, "teamId") && AsNumber(v, x)) who.team_id = int(x);
      else if (!std::strcmp(k, "roleId") && AsNumber(v, x)) who.role_id = int(x);
      else if (!std::strcmp(k, "jerseyNumber")) {
        if (v.type == 0x02) who.jersey = reinterpret_cast<const char*>(v.data + 4);
        else if (AsNumber(v, x)) who.jersey = std::to_string(int(x));
      } else if (!std::strcmp(k, "jointIds")) AsBinary(v, ids, nid);
      else if (!std::strcmp(k, "positions")) AsBinary(v, pos, npos);
      else if (!std::strcmp(k, "centerOfMass")) AsBinary(v, com, ncom);
      return true;
    }, error);
    if (!ok) return false;
    if (ids && pos) {
      const size_t count = std::min(nid, npos / 12);
      for (size_t j = 0; j < count; ++j) {
        const int id = ids[j];
        if (id < 0 || id >= 29) continue;
        float xyz[3];
        std::memcpy(xyz, pos + j * 12, 12);
        if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) || !std::isfinite(xyz[2])) continue;
        who.joints[id] = {xyz[0], xyz[1], xyz[2]};
        who.valid[id] = true;
      }
    }
    if (com && ncom >= 12) {
      float xyz[3];
      std::memcpy(xyz, com, 12);
      if (std::isfinite(xyz[0]) && std::isfinite(xyz[1]) && std::isfinite(xyz[2])) {
        who.com = {xyz[0], xyz[1], xyz[2]};
        who.has_com = true;
      }
    }
    out.people.push_back(std::move(who));
    return true;
  };
  bool ok = ForEach(doc, doc_size, [&](const char* k, const Value& v) {
    double x;
    if (!std::strcmp(k, "time") && AsNumber(v, x)) { out.time_us = v.type == 0x12 ? rd_i64(v.data) : int64_t(x); have_time = true; }
    else if (!std::strcmp(k, "personFrameData") && v.type == 0x03) {
      return ForEach(v.data, v.size, [&](const char* k2, const Value& v2) {
        if (std::strcmp(k2, "people") || v2.type != 0x04) return true;
        return ForEach(v2.data, v2.size, [&](const char*, const Value& item) {
          return item.type == 0x03 ? person(item.data, item.size) : true;
        }, error);
      }, error);
    }
    return true;
  }, error);
  if (!ok) return false;
  if (!have_time) { error = "frame has no time"; return false; }
  return true;
}

// ---------------------------------------------------------------- 姿勢の変換
namespace {
using Mat = std::array<double, 9>;
Mat Mul(const Mat& a, const Mat& b) {
  Mat r{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r[i * 3 + j] += a[i * 3 + k] * b[k * 3 + j];
  return r;
}
// HUMA(Y-up) ↔ UE(Z-up): M = X,Z,Y の入れ替え。UE の回転 = M R M。
Mat ToUe(const Mat& r) {
  return {r[0], r[2], r[1], r[6], r[8], r[7], r[3], r[5], r[4]};
}
void ToQuat(const Mat& m, float* q) {  // xyzw（hawkeye_to_huma_runtime.py と同じ分岐）
  const double tr = m[0] + m[4] + m[8];
  double x, y, z, w;
  if (tr > 0) {
    double s = std::sqrt(std::max(tr + 1, 1e-12)) * 2;
    x = (m[7] - m[5]) / s; y = (m[2] - m[6]) / s; z = (m[3] - m[1]) / s; w = 0.25 * s;
  } else if (m[0] >= m[4] && m[0] >= m[8]) {
    double s = std::sqrt(std::max(1 + m[0] - m[4] - m[8], 1e-12)) * 2;
    x = 0.25 * s; y = (m[1] + m[3]) / s; z = (m[2] + m[6]) / s; w = (m[7] - m[5]) / s;
  } else if (m[4] >= m[8]) {
    double s = std::sqrt(std::max(1 + m[4] - m[0] - m[8], 1e-12)) * 2;
    x = (m[1] + m[3]) / s; y = 0.25 * s; z = (m[5] + m[7]) / s; w = (m[2] - m[6]) / s;
  } else {
    double s = std::sqrt(std::max(1 + m[8] - m[0] - m[4], 1e-12)) * 2;
    x = (m[2] + m[6]) / s; y = (m[5] + m[7]) / s; z = 0.25 * s; w = (m[3] - m[1]) / s;
  }
  const double l = std::sqrt(x * x + y * y + z * z + w * w);
  q[0] = float(x / l); q[1] = float(y / l); q[2] = float(z / l); q[3] = float(w / l);
}
}  // namespace

void ToUePose(const float* ik_rot, int ik_joints, const float* root_m, const Profile& profile, Pose& out) {
  out.quat_xyzw.assign(size_t(kHumaJoints) * 4, 0.f);
  const Mat root_to_model = {0, 0, -1, 0, 1, 0, 1, 0, 0};  // engine.cpp と同じ
  for (int j = 0; j < kHumaJoints; ++j) {
    float* q = out.quat_xyzw.data() + j * 4;
    if (j < ik_joints && j < 26) {
      Mat r;
      for (int k = 0; k < 9; ++k) r[k] = ik_rot[j * 9 + k];
      if (j == 0) r = Mul(root_to_model, r);
      ToQuat(ToUe(r), q);
    } else if (j >= 26) {
      std::memcpy(q, kFixedJointQuatUe[j - 26], sizeof(float) * 4);
    } else {
      q[3] = 1.f;
    }
  }
  // 腰: root(フィールド Z-up, m) → UE cm (x, −y, z)×100、β 適用後の腰の静止位置を引く
  out.hip_displacement_cm = {root_m[0] * 100.f - profile.hip_rest_ue_cm[0],
                             -root_m[1] * 100.f - profile.hip_rest_ue_cm[1],
                             root_m[2] * 100.f - profile.hip_rest_ue_cm[2]};
}

// ---------------------------------------------------------------- 逐次ソルバ
void MakeIkInputs(const std::vector<std::array<float, 87>>& frames, int window, std::vector<float>& centered,
                  std::vector<float>& midhip) {
  centered.assign(size_t(window) * 87, 0.f);
  midhip.assign(size_t(window) * 3, 0.f);
  const int count = int(frames.size());
  for (int k = 0; k < window; ++k) {
    const int src = std::max(0, count - window + k);  // 先頭でクランプ
    const float* frame = frames[size_t(src)].data();
    const float* mid = frame + kMidHipBody29 * 3;
    for (int j = 0; j < 29; ++j)
      for (int a = 0; a < 3; ++a) centered[size_t(k) * 87 + j * 3 + a] = frame[j * 3 + a] - mid[a];
    for (int a = 0; a < 3; ++a) midhip[size_t(k) * 3 + a] = mid[a];
  }
}

LiveSolver::LiveSolver(IkModel& model, SolverOptions options) : model_(model), options_(options) {
  window_ = model_.Window();
  ik_joints_ = model_.Joints();
}

// ライブの trackId はデータセットの trackId と一致しないことがある。
// 同じ trackId があればそれ、なければ roleId と背番号が合う未使用の体型に割り当てる。
// 試合中に trackId が振り直される（ラウンドの合間など）ので、しばらく映っていない trackId が
// 持っている体型は「空き」とみなし、新しい trackId に付け替える。
int LiveSolver::ResolveProfile(const Person& person, const std::map<int, Profile>& profiles, int64_t now_us,
                               double stale_us) {
  auto known = alias_.find(person.track_id);
  if (known != alias_.end()) return known->second;
  int pick = -1;
  if (profiles.count(person.track_id)) {
    pick = person.track_id;
  } else {
    const int jersey = person.jersey.empty() ? 0 : std::atoi(person.jersey.c_str());
    auto active = [&](int live_track) {
      auto s = seen_.find(live_track);
      return s != seen_.end() && std::abs(double(now_us - s->second)) <= stale_us;
    };
    auto used = [&](int key) {
      for (const auto& a : alias_)
        if (a.second == key && a.first != person.track_id && active(a.first)) return true;
      return false;
    };
    for (const auto& p : profiles) {
      if (!used(p.first) && p.second.role_id == person.role_id && p.second.jersey_number == jersey) { pick = p.first; break; }
    }
    if (pick < 0) {  // 背番号が合わないとき: roleId が同じ未使用の体型が 1 つだけならそれ
      int count = 0, last = -1;
      for (const auto& p : profiles) {
        if (!used(p.first) && p.second.role_id == person.role_id) { ++count; last = p.first; }
      }
      if (count == 1) pick = last;
    }
  }
  if (pick >= 0) {
    // 同じ体型を持っていた古い trackId の対応を外す（1 つの体型に描くのは 1 人だけ）
    for (auto it = alias_.begin(); it != alias_.end();) {
      if (it->second == pick && it->first != person.track_id) {
        tracks_.erase(it->first);
        it = alias_.erase(it);
      } else {
        ++it;
      }
    }
    alias_[person.track_id] = pick;
  }
  return pick;
}

std::vector<Pose> LiveSolver::Push(const Frame& frame, const std::map<int, Profile>& profiles, std::string& error) {
  std::vector<Pose> result;
  const double step_us = 1e6 / std::max(1.f, options_.nominal_fps);
  const double stale_us = step_us * (options_.max_gap_frames + 1);
  for (const Person& person : frame.people) seen_[person.track_id] = frame.time_us;
  for (const Person& person : frame.people) {
    const int profile_key = ResolveProfile(person, profiles, frame.time_us, stale_us);
    if (profile_key < 0) continue;  // 体型を用意していない人物は描かない
    auto prof = profiles.find(profile_key);
    TrackState& st = tracks_[person.track_id];
    // 長い欠損の後や時刻が戻ったとき（ループ）は窓をリセット
    if (st.has_last && (double(frame.time_us - st.last_time_us) > step_us * (options_.max_gap_frames + 1) ||
                        frame.time_us <= st.last_time_us)) {
      st = TrackState{};
    }
    std::array<float, 87> body{};
    int have = 0;
    for (int b = 0; b < 29; ++b) {
      const int c = kCoco29ToBody29[b];
      if (person.valid[size_t(c)]) {
        for (int k = 0; k < 3; ++k) body[size_t(b * 3 + k)] = person.joints[size_t(c)][size_t(k)];
        ++have;
      } else if (st.has_last) {
        for (int k = 0; k < 3; ++k) body[size_t(b * 3 + k)] = st.last[size_t(b * 3 + k)];  // 欠けた関節は直前の値
        ++have;
      }
    }
    if (have < 29) continue;  // 初回で関節が欠けている
    st.last = body;
    st.has_last = true;
    st.last_time_us = frame.time_us;
    st.window.push_back(body);
    if (int(st.window.size()) > window_) st.window.erase(st.window.begin());

    MakeIkInputs(st.window, window_, centered_, midhip_);
    rot_.assign(size_t(ik_joints_) * 9, 0.f);
    root_.assign(3, 0.f);
    if (!model_.Run(centered_.data(), midhip_.data(), rot_.data(), root_.data(), error)) continue;
    Pose pose;
    pose.track_id = profile_key;  // 表示側（UE のプレイヤー）が持っている trackId
    pose.time_us = frame.time_us;
    ToUePose(rot_.data(), ik_joints_, root_.data(), prof->second, pose);
    pose.com_m = person.com;
    pose.has_com = person.has_com;
    // 前のフレームと符号をそろえる（補間で遠回りしないように）
    if (st.prev_quat.size() == pose.quat_xyzw.size()) {
      for (size_t j = 0; j < size_t(kHumaJoints); ++j) {
        float* q = pose.quat_xyzw.data() + j * 4;
        const float* p = st.prev_quat.data() + j * 4;
        if (q[0] * p[0] + q[1] * p[1] + q[2] * p[2] + q[3] * p[3] < 0)
          for (int k = 0; k < 4; ++k) q[k] = -q[k];
      }
    }
    st.prev_quat = pose.quat_xyzw;
    result.push_back(std::move(pose));
  }
  return result;
}

}  // namespace hawkeye_live
