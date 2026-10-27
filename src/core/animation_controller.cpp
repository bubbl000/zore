#include "core/animation_controller.h"

namespace nv {

const char* AnimStateName(AnimState state) {
  switch (state) {
    case AnimState::kStopped:
      return "stopped";
    case AnimState::kPlaying:
      return "playing";
    case AnimState::kPaused:
      return "paused";
    case AnimState::kEnded:
      return "ended";
  }
  return "stopped";
}

void AnimationController::Reset() {
  state_ = AnimState::kStopped;
  index_ = 0;
  loops_ = 0;
  paused_for_minimize_ = false;
}

void AnimationController::Start(size_t frame_count, uint32_t loop_count) {
  (void)loop_count;  // The loop limit is applied by Advance().
  Reset();
  if (frame_count < 2) return;
  state_ = AnimState::kPlaying;
}

void AnimationController::PlayFrom(size_t index) {
  index_ = index;
  loops_ = 0;
  paused_for_minimize_ = false;
  state_ = AnimState::kPlaying;
}

bool AnimationController::Toggle(size_t frame_count) {
  if (frame_count < 2) return false;
  if (state_ == AnimState::kEnded) {
    // Replay from the first frame instead of continuing at the end (坑 6).
    PlayFrom(0);
    return true;
  }
  if (state_ == AnimState::kPlaying) {
    state_ = AnimState::kPaused;
    return false;
  }
  if (index_ >= frame_count) index_ = 0;
  state_ = AnimState::kPlaying;
  paused_for_minimize_ = false;
  return true;
}

bool AnimationController::Pause() {
  if (state_ != AnimState::kPlaying) return false;
  state_ = AnimState::kPaused;
  return true;
}

bool AnimationController::Resume(size_t frame_count) {
  if (frame_count < 2) return false;
  if (state_ == AnimState::kPlaying) return true;
  if (state_ == AnimState::kEnded) {
    PlayFrom(0);
    return true;
  }
  if (index_ >= frame_count) index_ = 0;
  state_ = AnimState::kPlaying;
  paused_for_minimize_ = false;
  return true;
}

AnimStep AnimationController::Advance(size_t frame_count, uint32_t loop_count) {
  AnimStep step;
  step.index = index_;
  if (state_ != AnimState::kPlaying || frame_count < 2) return step;

  if (index_ + 1 >= frame_count) {
    // Last frame of a pass. A finite loop_count stops here instead of wrapping,
    // so kEnded really sits on the final frame (規劃/22/12 §3.2, 坑 5).
    if (loop_count != 0 && loops_ + 1 >= loop_count) {
      state_ = AnimState::kEnded;
      step.ended = true;
      return step;
    }
    index_ = 0;
    ++loops_;
  } else {
    ++index_;
  }
  step.show = true;
  step.index = index_;
  return step;
}

AnimStep AnimationController::Step(size_t frame_count, int delta) {
  AnimStep step;
  step.index = index_;
  if (frame_count < 2) return step;
  if (state_ == AnimState::kPlaying) {
    state_ = AnimState::kPaused;
    paused_for_minimize_ = false;
  }
  const long long count = static_cast<long long>(frame_count);
  const long long current = static_cast<long long>(index_);
  const long long movement = delta < 0 ? -1 : 1;
  index_ = static_cast<size_t>((current + count + movement) % count);
  if (state_ == AnimState::kStopped) state_ = AnimState::kPaused;
  step.show = true;
  step.index = index_;
  return step;
}

void AnimationController::SetFrame(size_t index) {
  index_ = index;
}

void AnimationController::FinishAt(size_t index) {
  index_ = index;
  loops_ = 0;
  paused_for_minimize_ = false;
  state_ = AnimState::kEnded;
}

bool AnimationController::OnMinimized() {
  paused_for_minimize_ = state_ == AnimState::kPlaying;
  if (!paused_for_minimize_) return false;
  state_ = AnimState::kPaused;
  return true;
}

bool AnimationController::OnRestored(size_t frame_count) {
  if (!paused_for_minimize_) return false;
  paused_for_minimize_ = false;
  if (frame_count < 2) {
    state_ = AnimState::kStopped;
    return false;
  }
  state_ = AnimState::kPlaying;
  return true;
}

}  // namespace nv
