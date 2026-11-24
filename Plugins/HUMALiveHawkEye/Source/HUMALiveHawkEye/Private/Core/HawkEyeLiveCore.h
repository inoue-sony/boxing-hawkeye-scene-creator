// Hawk-Eye フレーム BSON → HUMA 姿勢（UE の huma.runtime と同じ表現）を 1 フレームずつ作る、UE 非依存の中核部。
// UE 側（HUMALiveHawkEyeComponent）とテスト（Linux の g++）の両方から使う。
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hawkeye_live {

// ---------- Hawk-Eye BSON ----------
struct Person {
  int track_id = 0;
  int team_id = -1;
  int role_id = -1;
  std::string jersey;
  // COCO_29 の関節ごとの位置（フィールド座標、Z-up、m）。届いていない関節は valid=false。
  std::array<std::array<float, 3>, 29> joints{};
  std::array<bool, 29> valid{};
  // Hawk-Eye の重心（centerOfMass、フィールド座標 m）
  std::array<float, 3> com{};
  bool has_com = false;
};
struct Frame {
  int64_t time_us = 0;
  std::vector<Person> people;
};

// snappy の raw 形式を展開する。圧縮されていない（先頭が BSON の長さとして正しい）ときは false を返す。
bool SnappyDecompressRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);
// 1 フレーム分のファイル内容（snappy 圧縮 or 非圧縮 BSON）を読む。
bool ParseFrame(const uint8_t* data, size_t size, Frame& out, std::string& error);

// ---------- 因果 IK（ik_huma_causal_w9.onnx）
// 推論の実行部。UE では NNE（NNERuntimeORTCpu）、テストでは huma_ik の C API で実装する。
class IkModel {
 public:
  virtual ~IkModel() = default;
  virtual int Window() const = 0;  // 9
  virtual int Joints() const = 0;  // 26
  // centered: (W, 29, 3) を MidHip で中心化した値、midhip: (W, 3)。rot: (Joints, 3, 3)、root: (3)。
  virtual bool Run(const float* centered, const float* midhip, float* rot, float* root, std::string& error) = 0;
};

// 直近のフレーム列（古い順、W 未満なら先頭のフレームで埋める）から、モデルの入力を作る（huma_ik.cpp の run_windows と同じ）。
void MakeIkInputs(const std::vector<std::array<float, 87>>& frames, int window, std::vector<float>& centered,
                  std::vector<float>& midhip);

// 選手ごとに事前に決めておく値（体型は固定）。
struct Profile {
  int track_id = 0;
  int team_id = 0, role_id = 0, jersey_number = 0;  // UE 側の値（チームの外見などに使う）
  std::vector<float> betas;                          // 576
  std::array<float, 3> hip_rest_ue_cm{};             // M · J_hip(β)。hip 変位 = root(UE cm) − これ
};

// UE に渡す 1 人 1 フレーム分の姿勢。huma.runtime の motion.bin の 1 フレームと同じ並び。
struct Pose {
  int track_id = 0;
  int64_t time_us = 0;
  std::vector<float> quat_xyzw;  // 67 × 4（UE コンポーネント空間の親ローカル回転）
  std::array<float, 3> hip_displacement_cm{};
  std::array<float, 3> com_m{};  // Hawk-Eye の重心（フィールド座標 Z-up、m）
  bool has_com = false;
};

struct SolverOptions {
  int max_gap_frames = 15;   // これより長く届かなかったら窓をリセットする
  float nominal_fps = 50.f;  // 欠損の長さを数えるときの基準
};

// 届いたフレームを順に渡すと、映っている選手ごとの姿勢を返す。スレッドセーフではない（1 スレッドで使う）。
class LiveSolver {
 public:
  LiveSolver(IkModel& model, SolverOptions options = {});
  // 戻り値: 推論した選手の姿勢。error に失敗の理由（その選手だけ飛ばす）。
  std::vector<Pose> Push(const Frame& frame, const std::map<int, Profile>& profiles, std::string& error);
  // シーク時に呼ぶ。人物の対応付けも捨てて、読み直した先のデータで決め直す
  void Reset() { tracks_.clear(); alias_.clear(); seen_.clear(); }

 private:
  struct TrackState {
    std::vector<std::array<float, 87>> window;  // BODY29 順、直近 W フレーム
    std::array<float, 87> last{};
    bool has_last = false;
    int64_t last_time_us = 0;
    std::vector<float> prev_quat;  // 符号をそろえるため
  };
  IkModel& model_;
  SolverOptions options_;
  int window_ = 9, ik_joints_ = 26;
  std::map<int, TrackState> tracks_;
  std::map<int, int> alias_;      // ライブの trackId → 体型（データセット）の trackId
  std::map<int, int64_t> seen_;   // ライブの trackId → 最後に映った時刻（µs）
  int ResolveProfile(const Person& person, const std::map<int, Profile>& profiles, int64_t now_us, double stale_us);
  std::vector<float> rot_, root_, centered_, midhip_;
};

// IK の出力（ik_joints × 3×3、HUMA モデル座標の親ローカル）と root（フィールド m）を UE の表現にする。
void ToUePose(const float* ik_rot, int ik_joints, const float* root_m, const Profile& profile, Pose& out);

// COCO_29（Hawk-Eye）→ BODY_29（npb_huma_ik）の並び替え。
extern const int kCoco29ToBody29[29];
constexpr int kHumaJoints = 67;
constexpr int kMidHipBody29 = 12;

}  // namespace hawkeye_live
