#pragma once

#include <cstddef>

// rf::CameraController and the rf::Camera in it (Norbyte's, in
// GameDefinitions/Components/Camera.h), as the macOS game's code has them
// (Layout/GameShapes.cpp says which code). Norbyte's declarations are
// asserted to give these offsets (Layout/DefinitionChecks.cpp). Scripts
// reach a controller through the camera component's Controller.

namespace bg3se::mac {

// ls::CameraComponent (Norbyte's CameraComponent, the same header), the
// head of it: the type of the master behaviour in one byte, the controller,
// and, after the index of the exposure setting and a flag, whether the
// camera accepts input.
constexpr size_t kCameraComponentBehaviorType = 0x00;
constexpr size_t kCameraComponentController = 0x08;
constexpr size_t kCameraComponentAcceptsInput = 0x15;
constexpr size_t kCameraComponentSize = 0xb0;

// ls::Transform (Norbyte's Transform, CoreLib/Base/BaseTypes.h), as the
// controller's SetWorldTransform copies it: the rotation, the translation,
// the scale.
constexpr size_t kTransformTranslate = 0x10;
constexpr size_t kTransformScale = 0x1c;
constexpr size_t kTransformSize = 0x28;

// The controller: its four flags right after the vtable pointer, the field
// of view, the near and far planes and the aspect ratio, the world
// transform and the point looked at, a vector, the orthographic bounds at
// 0x60, the ID, the camera at 0x80, and the eight corners of the frustum,
// which end the object.
constexpr size_t kCameraViewDirty = 0x08;
constexpr size_t kCameraProjectionDirty = 0x09;
constexpr size_t kCameraOrthographic = 0x0a;
constexpr size_t kCameraOffCenter = 0x0b;
constexpr size_t kCameraFov = 0x0c;
constexpr size_t kCameraNearPlane = 0x10;
constexpr size_t kCameraFarPlane = 0x14;
constexpr size_t kCameraAspectRatio = 0x18;
constexpr size_t kCameraWorldTransform = 0x1c;      // ls::Transform: rotation, translation, scale
constexpr size_t kCameraLookAt = 0x44;
constexpr size_t kCameraVector = 0x50;              // Norbyte's field_58
constexpr size_t kCameraOrtho = 0x60;               // left, right, bottom, top
constexpr size_t kCameraId = 0x70;
constexpr size_t kCameraCamera = 0x80;
constexpr size_t kCameraFrustumPoints = 0x2a0;      // eight Vector3f
constexpr size_t kCameraControllerSize = 0x300;

// The camera: the view matrix and its inverse, the projection and its
// inverse, the product of the two and that of the projection with the
// view's rotation, then ten planes. The camera's own functions compute the
// first six planes, the controller's CalculateFrustumPoints the other four.
constexpr size_t kCameraView = 0x00;
constexpr size_t kCameraInvView = 0x40;
constexpr size_t kCameraProjection = 0x80;
constexpr size_t kCameraInvProjection = 0xc0;
constexpr size_t kCameraViewProjection = 0x100;
constexpr size_t kCameraOffsetViewProjection = 0x140;
constexpr size_t kCameraCullingPlanes = 0x180;      // ten Vector4f
constexpr size_t kCameraComputedPlanes = 6;
constexpr size_t kCameraSize = 0x220;

}
