// Frame loop: Wait/Begin/End with strict call-order tracking,
// fixed-period prediction (feedback refinement arrives in Phase 5),
// and projection-layer intake validation (GPU composite is Phase 5).

#include "runtime.h"
#include "streamer.h"
#include "vk_context.h"

#include <cstdlib>

namespace
{

// Minimal layer intake ("squasher seam"): validate one layer header,
// retain the count for Phase 5. Projection layers are accepted (depth
// subimages noted and ignored); anything else is a loud refusal.
XrResult validate_layer(const XrCompositionLayerBaseHeader * layer, uint32_t & projections)
{
	if (layer == nullptr)
		return XR_ERROR_VALIDATION_FAILURE;
	switch (layer->type)
	{
		case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
			auto * proj = reinterpret_cast<const XrCompositionLayerProjection *>(layer);
			if (proj->space == nullptr)
				return XR_ERROR_HANDLE_INVALID;
			if (proj->viewCount > 0 && proj->views == nullptr)
				return XR_ERROR_VALIDATION_FAILURE;
			for (uint32_t i = 0; i < proj->viewCount; ++i)
			{
				if (proj->views[i].type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW)
					return XR_ERROR_VALIDATION_FAILURE;
				if (proj->views[i].subImage.swapchain == nullptr)
					return XR_ERROR_HANDLE_INVALID;
			}
			// Depth subimages are accepted but ignored in v1.
			if (proj->next != nullptr)
				wivrn_macos::log("EndFrame: ignoring chained layer data in v1");
			projections += 1;
			return XR_SUCCESS;
		}
		case XR_TYPE_COMPOSITION_LAYER_QUAD:
		case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR:
		case XR_TYPE_COMPOSITION_LAYER_CUBE_KHR:
		case XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR:
		case XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR:
			wivrn_macos::log("EndFrame: layer type %d unsupported in v1 (projection only)", (int)layer->type);
			return XR_ERROR_FEATURE_UNSUPPORTED;
		default:
			return XR_ERROR_VALIDATION_FAILURE;
	}
}

} // namespace

extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL
	wivrn_macos_WaitFrame(XrSession session, const XrFrameWaitInfo * frameWaitInfo, XrFrameState * frameState)
	{
		if (session == nullptr || frameWaitInfo == nullptr || frameState == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (frameWaitInfo->type != XR_TYPE_FRAME_WAIT_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (frameState->type != XR_TYPE_FRAME_STATE)
			return XR_ERROR_VALIDATION_FAILURE;
		if (session->data.state == XR_SESSION_STATE_EXITING || session->data.state == XR_SESSION_STATE_LOSS_PENDING)
			return XR_ERROR_SESSION_LOST;
		if (session->data.state != XR_SESSION_STATE_SYNCHRONIZED && session->data.state != XR_SESSION_STATE_VISIBLE &&
		    session->data.state != XR_SESSION_STATE_FOCUSED)
			return XR_ERROR_SESSION_NOT_READY;
		if (session->data.lastCall != wivrn_macos::Session::FrameCall::None &&
		    session->data.lastCall != wivrn_macos::Session::FrameCall::End)
			return XR_ERROR_CALL_ORDER_INVALID;

		// Fixed-period prediction, phase-locked to now (no drift pile-up).
		int64_t now = wivrn_macos::now_ns();
		int64_t predicted = session->data.lastPredictedNs > now ? session->data.lastPredictedNs : now;
		predicted += session->data.periodNs;
		session->data.lastPredictedNs = predicted;
		session->data.frameIndex += 1;
		session->data.lastCall = wivrn_macos::Session::FrameCall::Wait;

		frameState->predictedDisplayTime = predicted;
		frameState->predictedDisplayPeriod = session->data.periodNs;
		frameState->shouldRender = XR_TRUE;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_BeginFrame(XrSession session, const XrFrameBeginInfo * frameBeginInfo)
	{
		if (session == nullptr || frameBeginInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (frameBeginInfo->type != XR_TYPE_FRAME_BEGIN_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (session->data.lastCall != wivrn_macos::Session::FrameCall::Wait)
			return XR_ERROR_CALL_ORDER_INVALID;
		session->data.lastCall = wivrn_macos::Session::FrameCall::Begin;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL wivrn_macos_EndFrame(XrSession session, const XrFrameEndInfo * frameEndInfo)
	{
		if (session == nullptr || frameEndInfo == nullptr)
			return XR_ERROR_HANDLE_INVALID;
		if (frameEndInfo->type != XR_TYPE_FRAME_END_INFO)
			return XR_ERROR_VALIDATION_FAILURE;
		if (session->data.lastCall != wivrn_macos::Session::FrameCall::Begin)
			return XR_ERROR_CALL_ORDER_INVALID;
		if (frameEndInfo->displayTime <= 0)
			return XR_ERROR_TIME_INVALID;
		if (frameEndInfo->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
			return XR_ERROR_FEATURE_UNSUPPORTED;
		uint32_t projections = 0;
		for (uint32_t i = 0; i < frameEndInfo->layerCount; ++i)
		{
			XrResult res = validate_layer(frameEndInfo->layers[i], projections);
			if (res != XR_SUCCESS)
				return res;
		}
		session->data.lastLayerCount = projections;
		session->data.lastCall = wivrn_macos::Session::FrameCall::End;
		// Pixels-to-wire (no-op without a headset; see streamer). Headless
		// test runs opt out via WIVRN_MACOS_HEADLESS so they never bind
		// ports or advertise Bonjour.
		static bool headless = std::getenv("WIVRN_MACOS_HEADLESS") != nullptr;
		if (!headless)
			wivrn_macos::Streamer::singleton().end_frame(&session->data, frameEndInfo, frameEndInfo->displayTime);
		return XR_SUCCESS;
	}

} // extern "C"
