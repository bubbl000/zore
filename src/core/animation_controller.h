#pragma once

#include <cstddef>
#include <cstdint>

namespace nv {

// Playback state of one animated image (規劃/22/12 §3.2). The controller is a
// pure state machine: it never talks to a window and never touches pixels, so
// every transition is unit-tested without a D3D device or a timer. The app owns
// the WM_TIMER that drives Advance() and the Renderer::SetImage that shows the
// returned frame.
enum class AnimState {
  kStopped = 0,  // no animation loaded (or the file was closed)
  kPlaying,      // the frame timer is running
  kPaused,       // a frame is shown, the timer is not running
  kEnded,        // finite loop finished on the last frame; Play restarts at 0
};

const char* AnimStateName(AnimState state);

struct AnimStep {
  // True when a new frame must be uploaded. False means "no visual change"
  // (playback finished, paused, or the file has a single frame).
  bool show = false;
  size_t index = 0;
  // Playback reached the end of a finite loop and stopped on the last frame.
  bool ended = false;
};

class AnimationController {
 public:
  void Reset();
  // Starts playback from the first frame. A single-frame image stays stopped
  // (坑 15: the pill must not appear for a static file).
  void Start(size_t frame_count, uint32_t loop_count);

  // Play/pause. Playing from kEnded replays from frame 0 (坑 6). Returns true
  // when the frame timer should now run.
  bool Toggle(size_t frame_count);
  bool Pause();
  // Resumes a paused animation. kEnded restarts from the first frame.
  bool Resume(size_t frame_count);

  // Timer tick: moves to the next frame, wrapping and counting loops.
  AnimStep Advance(size_t frame_count, uint32_t loop_count);
  // Manual stepping (`,`/`.`): always pauses first and wraps (規劃/22/12 §3.3).
  AnimStep Step(size_t frame_count, int delta);
  // Lets the app sync an index it changed itself (e.g. after a failed upload).
  void SetFrame(size_t index);
  // Test hook (--force-anim ended): parks the controller on the last frame in
  // the finished state so screenshots/dumps do not depend on loop timing.
  void FinishAt(size_t index);

  // Minimise auto-pauses and remembers whether playback was active (坑 8/9).
  bool OnMinimized();
  bool OnRestored(size_t frame_count);

  AnimState state() const { return state_; }
  bool playing() const { return state_ == AnimState::kPlaying; }
  bool ended() const { return state_ == AnimState::kEnded; }
  bool stopped() const { return state_ == AnimState::kStopped; }
  size_t index() const { return index_; }
  uint32_t loops_done() const { return loops_; }
  // True after OnMinimized paused an actively playing animation.
  bool paused_for_minimize() const { return paused_for_minimize_; }

 private:
  void PlayFrom(size_t index);

  AnimState state_ = AnimState::kStopped;
  size_t index_ = 0;
  uint32_t loops_ = 0;
  bool paused_for_minimize_ = false;
};

}  // namespace nv
