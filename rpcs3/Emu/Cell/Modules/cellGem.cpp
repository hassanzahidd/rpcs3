#include "stdafx.h"
#include "cellGem.h"
#include "cellCamera.h"

#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/PPUModule.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Io/MouseHandler.h"
#include "Emu/Io/PadHandler.h"
#include "Emu/Io/gem_config.h"
#include "Emu/system_config.h"
#include "Emu/System.h"
#include "Emu/IdManager.h"
#include "Emu/RSX/Overlays/overlay_cursor.h"
#include "Input/pad_thread.h"
#include "Input/ps_move_config.h"
#include "Input/ps_move_tracker.h"

#ifdef HAVE_LIBEVDEV
#include "Input/evdev_gun_handler.h"
#endif

#include <cmath> // for fmod
#include <type_traits>

LOG_CHANNEL(cellGem);

template <>
void fmt_class_string<gem_btn>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](gem_btn value)
	{
		switch (value)
		{
		case gem_btn::start: return "Start";
		case gem_btn::select: return "Select";
		case gem_btn::triangle: return "Triangle";
		case gem_btn::circle: return "Circle";
		case gem_btn::cross: return "Cross";
		case gem_btn::square: return "Square";
		case gem_btn::move: return "Move";
		case gem_btn::t: return "T";
		case gem_btn::count: return "Count";
		case gem_btn::x_axis: return "X-Axis";
		case gem_btn::y_axis: return "Y-Axis";
		}

		return unknown;
	});
}

template <>
void fmt_class_string<CellGemError>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED);
			STR_CASE(CELL_GEM_ERROR_ALREADY_INITIALIZED);
			STR_CASE(CELL_GEM_ERROR_UNINITIALIZED);
			STR_CASE(CELL_GEM_ERROR_INVALID_PARAMETER);
			STR_CASE(CELL_GEM_ERROR_INVALID_ALIGNMENT);
			STR_CASE(CELL_GEM_ERROR_UPDATE_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_UPDATE_NOT_STARTED);
			STR_CASE(CELL_GEM_ERROR_CONVERT_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_CONVERT_NOT_STARTED);
			STR_CASE(CELL_GEM_ERROR_WRITE_NOT_FINISHED);
			STR_CASE(CELL_GEM_ERROR_NOT_A_HUE);
		}

		return unknown;
	});
}

template <>
void fmt_class_string<CellGemStatus>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto error)
	{
		switch (error)
		{
			STR_CASE(CELL_GEM_NOT_CONNECTED);
			STR_CASE(CELL_GEM_SPHERE_NOT_CALIBRATED);
			STR_CASE(CELL_GEM_SPHERE_CALIBRATING);
			STR_CASE(CELL_GEM_COMPUTING_AVAILABLE_COLORS);
			STR_CASE(CELL_GEM_HUE_NOT_SET);
			STR_CASE(CELL_GEM_NO_VIDEO);
			STR_CASE(CELL_GEM_TIME_OUT_OF_RANGE);
			STR_CASE(CELL_GEM_NOT_CALIBRATED);
			STR_CASE(CELL_GEM_NO_EXTERNAL_PORT_DEVICE);
		}

		return unknown;
	});
}

template <>
void fmt_class_string<CellGemVideoConvertFormatEnum>::format(std::string& out, u64 arg)
{
	format_enum(out, arg, [](auto format)
	{
		switch (format)
		{
			STR_CASE(CELL_GEM_NO_VIDEO_OUTPUT);
			STR_CASE(CELL_GEM_RGBA_640x480);
			STR_CASE(CELL_GEM_YUV_640x480);
			STR_CASE(CELL_GEM_YUV422_640x480);
			STR_CASE(CELL_GEM_YUV411_640x480);
			STR_CASE(CELL_GEM_RGBA_320x240);
			STR_CASE(CELL_GEM_BAYER_RESTORED);
			STR_CASE(CELL_GEM_BAYER_RESTORED_RGGB);
			STR_CASE(CELL_GEM_BAYER_RESTORED_RASTERIZED);
		}

		return unknown;
	});
}

// last 4 out of 7 ports (7,6,5,4). index starts at 1
static u32 port_num(u32 gem_num)
{
	ensure(gem_num < CELL_GEM_MAX_NUM);
	return CELL_PAD_MAX_PORT_NUM - gem_num;
}

// last 4 out of 7 ports (6,5,4,3). index starts at 0
static u32 pad_num(u32 gem_num)
{
	ensure(gem_num < CELL_GEM_MAX_NUM);
	return (CELL_PAD_MAX_PORT_NUM - 1) - gem_num;
}

// **********************
// * HLE helper structs *
// **********************

#ifdef HAVE_LIBEVDEV
struct gun_handler
{
public:
	gun_handler() = default;

	static constexpr auto thread_name = "Evdev Gun Thread"sv;

	evdev_gun_handler handler{};
	atomic_t<u32> num_devices{0};

	void operator()()
	{
		if (g_cfg.io.move != move_handler::gun)
		{
			return;
		}

		while (thread_ctrl::state() != thread_state::aborting && !Emu.IsStopped())
		{
			const bool is_active = !Emu.IsPaused() && handler.is_init();

			if (is_active)
			{
				for (u32 i = 0; i < num_devices; i++)
				{
					std::scoped_lock lock(handler.mutex);
					handler.poll(i);
				}
			}

			thread_ctrl::wait_for(is_active ? 1000 : 10000);
		}
	}
};

using gun_thread = named_thread<gun_handler>;

#endif

cfg_gems g_cfg_gem_real;
cfg_fake_gems g_cfg_gem_fake;

struct gem_config_data
{
public:
	void operator()();

	static constexpr auto thread_name = "Gem Thread"sv;

	atomic_t<u8> state = 0;

	struct gem_color
	{
		ENABLE_BITWISE_SERIALIZATION;

		f32 r, g, b;

		gem_color() : r(0.0f), g(0.0f), b(0.0f) {}
		gem_color(f32 r_, f32 g_, f32 b_)
		{
			r = std::clamp(r_, 0.0f, 1.0f);
			g = std::clamp(g_, 0.0f, 1.0f);
			b = std::clamp(b_, 0.0f, 1.0f);
		}

		static inline const gem_color& get_default_color(u32 gem_num)
		{
			static const gem_color gold = gem_color(1.0f, 0.85f, 0.0f);
			static const gem_color green = gem_color(0.0f, 1.0f, 0.0f);
			static const gem_color red = gem_color(1.0f, 0.0f, 0.0f);
			static const gem_color pink = gem_color(0.9f, 0.0f, 0.5f);

			switch (gem_num)
			{
			case 0: return green;
			case 1: return gold;
			case 2: return red;
			case 3: return pink;
			default: fmt::throw_exception("unexpected gem_num %d", gem_num);
			}
		}
	};

	struct gem_controller
	{
		u32 status = CELL_GEM_STATUS_DISCONNECTED;         // Connection status (CELL_GEM_STATUS_DISCONNECTED or CELL_GEM_STATUS_READY)
		u32 ext_status = CELL_GEM_NO_EXTERNAL_PORT_DEVICE; // External port connection status
		u32 ext_id = 0;                                    // External device ID (type). For example SHARP_SHOOTER_DEVICE_ID
		u32 port = 0;                                      // Assigned port
		bool enabled_magnetometer = true;                  // Whether the magnetometer is enabled (probably used for additional rotational precision)
		bool calibrated_magnetometer = false;              // Whether the magnetometer is calibrated
		bool enabled_filtering = false;                    // Whether filtering is enabled
		bool enabled_tracking = false;                     // Whether tracking is enabled
		bool enabled_LED = false;                          // Whether the LED is enabled
		bool hue_set = false;                              // Whether the hue was set
		u8 rumble = 0;                                     // Rumble intensity
		gem_color sphere_rgb = {};                         // RGB color of the sphere LED
		u32 hue = 0;                                       // Tracking hue of the motion controller
		f32 distance_mm{3000.0f};                          // Distance from the camera in mm
		f32 radius{5.0f};                                  // Radius of the sphere in camera pixels
		bool radius_valid = true;                          // If the radius and distance of the sphere was computed.

		bool is_calibrating{false};                        // Whether or not we are currently calibrating
		u64 calibration_start_us{0};                       // The start timestamp of the calibration in microseconds
		u64 calibration_status_flags = 0;                  // The calibration status flags

		static constexpr u64 calibration_time_us = 500000; // The calibration supposedly takes 0.5 seconds (500000 microseconds)
	};

	CellGemAttribute attribute = {};
	CellGemVideoConvertAttribute vc_attribute = {};
	s32 video_data_out_size = -1;
	std::vector<u8> video_data_in;
	u64 runtime_status_flags = 0; // The runtime status flags
	bool enable_pitch_correction = false;
	u32 inertial_counter = 0;

	std::array<gem_controller, CELL_GEM_MAX_NUM> controllers;
	u32 connected_controllers = 0;
	atomic_t<bool> video_conversion_in_progress{false};
	atomic_t<bool> updating{false};
	u32 camera_frame{};
	u32 memory_ptr{};

	shared_mutex mtx;

	u64 start_timestamp_us = 0;

	// helper functions
	bool is_controller_ready(u32 gem_num) const
	{
		return controllers[gem_num].status == CELL_GEM_STATUS_READY;
	}

	void update_calibration_status()
	{
		std::scoped_lock lock(mtx);

		for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
		{
			gem_controller& controller = controllers[gem_num];
			if (!controller.is_calibrating) continue;

			bool controller_calibrated = true;

			// Request controller calibration
			if (g_cfg.io.move == move_handler::real)
			{
				std::lock_guard pad_lock(pad::g_pad_mutex);
				const auto handler = pad::get_current_handler();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));
				if (pad && pad->m_pad_handler == pad_handler::move)
				{
					if (!pad->move_data.calibration_requested || !pad->move_data.calibration_succeeded)
					{
						pad->move_data.calibration_requested = true;
						controller_calibrated = false;
					}
				}
			}

			// The calibration takes ~0.5 seconds on real hardware
			if ((get_guest_system_time() - controller.calibration_start_us) < gem_controller::calibration_time_us) continue;

			if (!controller_calibrated)
			{
				cellGem.warning("Reached calibration timeout but ps move controller %d is still calibrating", gem_num);
			}

			controller.is_calibrating = false;
			controller.calibration_start_us = 0;
			controller.calibration_status_flags = CELL_GEM_FLAG_CALIBRATION_SUCCEEDED | CELL_GEM_FLAG_CALIBRATION_OCCURRED;
			controller.calibrated_magnetometer = true;
			controller.enabled_tracking = true;

			// Reset controller calibration request
			if (g_cfg.io.move == move_handler::real)
			{
				std::lock_guard pad_lock(pad::g_pad_mutex);
				const auto handler = pad::get_current_handler();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));
				if (pad && pad->m_pad_handler == pad_handler::move)
				{
					pad->move_data.calibration_requested = false;
					pad->move_data.calibration_succeeded = false;
				}
			}
		}
	}

	void reset_controller(u32 gem_num)
	{
		if (gem_num >= CELL_GEM_MAX_NUM)
		{
			return;
		}

		gem_controller& controller = ::at32(controllers, gem_num);
		controller = {};
		controller.sphere_rgb = gem_color::get_default_color(gem_num);

		bool is_connected = false;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		{
			connected_controllers = 0;
			std::lock_guard lock(pad::g_pad_mutex);
			const auto handler = pad::get_current_handler();
			for (u32 i = 0; i < std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM); i++)
			{
				const auto& pad = ::at32(handler->GetPads(), pad_num(i));
				if (pad && pad->m_pad_handler == pad_handler::move && (pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
				{
					connected_controllers++;

					if (gem_num == i)
					{
						pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
						is_connected = true;
					}
				}
			}
			break;
		}
		case move_handler::fake:
		{
			connected_controllers = 0;
			std::lock_guard lock(pad::g_pad_mutex);
			const auto handler = pad::get_current_handler();
			for (u32 i = 0; i < std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM); i++)
			{
				const auto& pad = ::at32(handler->GetPads(), pad_num(i));
				if (pad && (pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
				{
					connected_controllers++;

					if (gem_num == i)
					{
						is_connected = true;
					}
				}
			}
			break;
		}
		case move_handler::mouse:
		case move_handler::raw_mouse:
		{
			auto& handler = g_fxo->get<MouseHandlerBase>();
			std::lock_guard mouse_lock(handler.mutex);

			// Make sure that the mouse handler is initialized
			handler.Init(std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM));

			const MouseInfo& info = handler.GetInfo();
			connected_controllers = std::min<u32>({ info.now_connect, attribute.max_connect, CELL_GEM_MAX_NUM });
			if (gem_num < connected_controllers)
			{
				is_connected = true;
			}
			break;
		}
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
		{
			gun_thread& gun = g_fxo->get<gun_thread>();
			std::scoped_lock lock(gun.handler.mutex);
			gun.num_devices = gun.handler.init() ? gun.handler.get_num_guns() : 0;
			connected_controllers = std::min<u32>(std::min<u32>(attribute.max_connect, CELL_GEM_MAX_NUM), gun.num_devices);

			if (gem_num < connected_controllers)
			{
				is_connected = true;
			}
			break;
		}
#endif
		case move_handler::null:
			break;
		}

		// Assign status and port number
		if (is_connected)
		{
			controller.status = CELL_GEM_STATUS_READY;
			controller.port = port_num(gem_num);
		}
	}

	void paint_spheres(CellGemVideoConvertFormatEnum output_format, u32 width, u32 height, u8* video_data_out, u32 video_data_out_size);

	gem_config_data()
	{
		load_configs();
	};

	SAVESTATE_INIT_POS(15);

	void save(utils::serial& ar)
	{
		ar(state);

		if (!state)
		{
			return;
		}

		const s32 version = GET_OR_USE_SERIALIZATION_VERSION(ar.is_writing(), cellGem);

		ar(attribute, vc_attribute, runtime_status_flags, enable_pitch_correction, inertial_counter);

		for (gem_controller& c : controllers)
		{
			ar(c.status, c.ext_status, c.ext_id, c.port, c.enabled_magnetometer, c.calibrated_magnetometer, c.enabled_filtering, c.enabled_tracking, c.enabled_LED, c.hue_set, c.rumble);

			// We need to add padding because we used bitwise serialization in version 1
			if (version < 2)
			{
				ar.add_padding(&gem_controller::rumble, &gem_controller::sphere_rgb);
			}

			ar(c.sphere_rgb, c.hue, c.distance_mm, c.radius, c.radius_valid, c.is_calibrating);

			if (version < 2)
			{
				ar.add_padding(&gem_controller::is_calibrating, &gem_controller::calibration_start_us);
			}

			ar(c.calibration_start_us);

			if (ar.is_writing() || version >= 2)
			{
				ar(c.calibration_status_flags);
			}
		}

		ar(connected_controllers, updating, camera_frame, memory_ptr, start_timestamp_us);
	}

	gem_config_data(utils::serial& ar)
	{
		save(ar);
		load_configs();
	}

	static void load_configs()
	{
		if (!g_cfg_gem_real.load())
		{
			cellGem.notice("Could not load real gem config. Using defaults.");
		}

		if (!g_cfg_gem_fake.load())
		{
			cellGem.notice("Could not load fake gem config. Using defaults.");
		}

		cellGem.notice("Real gem config=\n", g_cfg_gem_real.to_string());
		cellGem.notice("Fake gem config=\n", g_cfg_gem_fake.to_string());
	}
};

extern std::pair<u32, u32> get_video_resolution(const CellCameraInfoEx& info);
extern u32 get_buffer_size_by_format(s32 format, s32 width, s32 height);

static inline s32 cellGemGetVideoConvertSize(s32 output_format)
{
	switch (output_format)
	{
	case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
		return 320 * 240 * 4;
	case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
		return 640 * 480 * 4;
	case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
		return 640 * 480 + 640 * 480 + 640 * 480;
	case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
		return 640 * 480 + 320 * 480 + 320 * 480;
	case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
		return 640 * 480 + 320 * 240 + 320 * 240;
	case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
	case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
	case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
		return 640 * 480;
	case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
		return 0;
	default:
		return -1;
	}
}

namespace gem
{
	struct gem_position
	{
	public:
		void set_position(f32 x, f32 y)
		{
			std::lock_guard lock(m_mutex);
			m_x = x;
			m_y = y;
		}
		void get_position(f32& x, f32& y)
		{
			std::lock_guard lock(m_mutex);
			x = m_x;
			y = m_y;
		}
	private:
		std::mutex m_mutex;
		f32 m_x = 0.0f;
		f32 m_y = 0.0f;
	};

	std::array<gem_position, CELL_GEM_MAX_NUM> positions {};

	bool convert_image_format(CellCameraFormat input_format, CellGemVideoConvertFormatEnum output_format,
	                          const std::vector<u8>& video_data_in, u32 width, u32 height,
	                          u8* video_data_out, u32 video_data_out_size)
	{
		if (output_format != CELL_GEM_NO_VIDEO_OUTPUT && !video_data_out)
		{
			return false;
		}

		const u32 required_in_size = get_buffer_size_by_format(static_cast<s32>(input_format), width, height);
		const s32 required_out_size = cellGemGetVideoConvertSize(output_format);

		if (video_data_in.size() != required_in_size)
		{
			cellGem.error("convert: in_size mismatch: required=%d, actual=%d", required_in_size, video_data_in.size());
			return false;
		}

		if (required_out_size < 0 || video_data_out_size != static_cast<u32>(required_out_size))
		{
			cellGem.error("convert: out_size unknown: required=%d, format %d", required_out_size, output_format);
			return false;
		}

		if (required_out_size == 0)
		{
			return false;
		}

		switch (output_format)
		{
		case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
		{
			switch (input_format)
			{
			case CELL_CAMERA_RAW8:
			{
				const u32 in_pitch = width;
				const u32 out_pitch = width * 4;

				for (u32 y = 0; y < height - 1; y += 2)
				{
					const u8* src = &video_data_in[y * in_pitch];
					const u8* src0 = src;
					const u8* src1 = src + in_pitch;

					u8* dst_row = video_data_out + y * out_pitch;
					u8* dst0 = dst_row;
					u8* dst1 = dst_row + out_pitch;

					for (u32 x = 0; x < width - 1; x += 2, src0 += 2, src1 += 2, dst0 += 8, dst1 += 8)
					{
						const u8 b  = src0[0];
						const u8 g0 = src0[1];
						const u8 g1 = src1[0];
						const u8 r  = src1[1];

						const u8 top[4] = { r, g0, b, 255 };
						const u8 bottom[4] = { r, g1, b, 255 };

						// Top-Left
						std::memcpy(dst0, top, 4);

						// Top-Right Pixel
						std::memcpy(dst0 + 4, top, 4);

						// Bottom-Left Pixel
						std::memcpy(dst1, bottom, 4);

						// Bottom-Right Pixel
						std::memcpy(dst1 + 4, bottom, 4);
					}
				}
				break;
			}
			case CELL_CAMERA_RGBA:
			{
				std::memcpy(video_data_out, video_data_in.data(), std::min<usz>(required_in_size, required_out_size));
				break;
			}
			default:
			{
				cellGem.error("Unimplemented: Converting %s to %s", input_format, output_format);
				std::memcpy(video_data_out, video_data_in.data(), std::min<usz>(required_in_size, required_out_size));
				return false;
			}
			}
			break;
		}
		case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
		{
			if (input_format == CELL_CAMERA_RAW8)
			{
				std::memcpy(video_data_out, video_data_in.data(), std::min<usz>(required_in_size, required_out_size));
			}
			else
			{
				cellGem.error("Unimplemented: Converting %s to %s", input_format, output_format);
				return false;
			}
			break;
		}
		case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
		case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
		case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
		case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
		case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
		case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
		{
			cellGem.error("Unimplemented: Converting %s to %s", input_format, output_format);
			return false;
		}
		case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
		{
			cellGem.trace("Ignoring frame conversion for CELL_GEM_NO_VIDEO_OUTPUT");
			break;
		}
		default:
		{
			cellGem.error("Trying to convert %s to %s", input_format, output_format);
			return false;
		}
		}

		return true;
	}
}

void gem_config_data::paint_spheres(CellGemVideoConvertFormatEnum output_format, u32 width, u32 height, u8* video_data_out, u32 video_data_out_size)
{
	if (!width || !height || !video_data_out || !video_data_out_size)
	{
		return;
	}

	struct sphere_information
	{
		f32 radius = 0.0f;
		s16 x = 0;
		s16 y = 0;
		u8 r = 0;
		u8 g = 0;
		u8 b = 0;
	};

	std::vector<sphere_information> sphere_info;
	{
		reader_lock lock(mtx);

		for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
		{
			const gem_config_data::gem_controller& controller = controllers[gem_num];
			if (!controller.radius_valid || controller.radius <= 0.0f) continue;

			f32 x, y;
			::at32(gem::positions, gem_num).get_position(x, y);

			const u8 r = static_cast<u8>(std::clamp(controller.sphere_rgb.r * 255.0f, 0.0f, 255.0f));
			const u8 g = static_cast<u8>(std::clamp(controller.sphere_rgb.g * 255.0f, 0.0f, 255.0f));
			const u8 b = static_cast<u8>(std::clamp(controller.sphere_rgb.b * 255.0f, 0.0f, 255.0f));

			sphere_info.push_back({ controller.radius, static_cast<s16>(x), static_cast<s16>(y), r, g, b });
		}
	}

	switch (output_format)
	{
	case CELL_GEM_RGBA_640x480: // RGBA output; 640*480*4-byte output buffer required
	{
		cellGem.trace("Painting spheres for CELL_GEM_RGBA_640x480");

		const u32 out_pitch = width * 4;

		for (const sphere_information& info : sphere_info)
		{
			const s32 x_begin = std::max(0, static_cast<s32>(std::floor(info.x - info.radius)));
			const s32 x_end = std::min<s32>(width, static_cast<s32>(std::ceil(info.x + info.radius)));
			const s32 y_begin = std::max(0, static_cast<s32>(std::floor(info.y - info.radius)));
			const s32 y_end = std::min<s32>(height, static_cast<s32>(std::ceil(info.y + info.radius)));

			for (s32 y = y_begin; y < y_end; y++)
			{
				u8* dst = video_data_out + y * out_pitch + x_begin * 4;

				for (s32 x = x_begin; x < x_end; x++, dst += 4)
				{
					const f32 distance = static_cast<f32>(std::sqrt(std::pow(info.x - x, 2) + std::pow(info.y - y, 2)));
					if (distance > info.radius) continue;

					dst[0] = info.r;
					dst[1] = info.g;
					dst[2] = info.b;
					dst[3] = 255;
				}
			}
		}

		break;
	}
	case CELL_GEM_BAYER_RESTORED: // Bayer pattern output, 640x480, gamma and white balance applied, output buffer required
	case CELL_GEM_RGBA_320x240: // RGBA output; 320*240*4-byte output buffer required
	case CELL_GEM_YUV_640x480: // YUV output; 640*480+640*480+640*480-byte output buffer required (contiguous)
	case CELL_GEM_YUV422_640x480: // YUV output; 640*480+320*480+320*480-byte output buffer required (contiguous)
	case CELL_GEM_YUV411_640x480: // YUV411 output; 640*480+320*240+320*240-byte output buffer required (contiguous)
	case CELL_GEM_BAYER_RESTORED_RGGB: // Restored Bayer output, 2x2 pixels rearranged into 320x240 RG1G2B
	case CELL_GEM_BAYER_RESTORED_RASTERIZED: // Restored Bayer output, R,G1,G2,B rearranged into 4 contiguous 320x240 1-channel rasters
	{
		cellGem.trace("Unimplemented: painting spheres for %s", output_format);
		break;
	}
	case CELL_GEM_NO_VIDEO_OUTPUT: // Disable video output
	{
		cellGem.trace("Ignoring painting spheres for CELL_GEM_NO_VIDEO_OUTPUT");
		break;
	}
	default:
	{
		cellGem.trace("Ignoring painting spheres for %d", static_cast<u32>(output_format));
		break;
	}
	}
}

void gem_config_data::operator()()
{
	cellGem.notice("Starting thread");

	while (thread_ctrl::state() != thread_state::aborting && !Emu.IsStopped())
	{
		while (!video_conversion_in_progress && thread_ctrl::state() != thread_state::aborting && !Emu.IsStopped())
		{
			if (state)
			{
				update_calibration_status();
			}

			thread_ctrl::wait_for(1000);
		}

		if (thread_ctrl::state() == thread_state::aborting || Emu.IsStopped())
		{
			return;
		}

		CellGemVideoConvertAttribute vc;
		{
			std::scoped_lock lock(mtx);
			vc = vc_attribute;
		}

		if (g_cfg.io.camera != camera_handler::qt)
		{
			video_conversion_in_progress = false;
			continue;
		}

		const auto& shared_data = g_fxo->get<gem_camera_shared>();

		if (gem::convert_image_format(shared_data.format, vc.output_format, video_data_in, shared_data.width, shared_data.height, vc_attribute.video_data_out ? vc_attribute.video_data_out.get_ptr() : nullptr, video_data_out_size))
		{
			cellGem.trace("Converted video frame of format %s to %s", shared_data.format.load(), vc.output_format.get());

			if (g_cfg.io.paint_move_spheres)
			{
				paint_spheres(vc.output_format, shared_data.width, shared_data.height, vc_attribute.video_data_out ? vc_attribute.video_data_out.get_ptr() : nullptr, video_data_out_size);
			}
		}

		video_conversion_in_progress = false;
	}
}

using gem_config = named_thread<gem_config_data>;

class gem_tracker
{
public:
	gem_tracker()
	{
	}

	bool is_busy()
	{
		return m_busy;
	}

	void wake_up()
	{
		m_wake_up.release(1);
		m_wake_up.notify_one();
	}

	void wait_for_result()
	{
		if (!m_done)
		{
			m_done.wait(0);
			m_done.release(0);
		}
	}

	bool set_image(u32 addr)
	{
		if (!addr)
			return false;

		auto& g_camera = g_fxo->get<camera_thread>();
		std::lock_guard lock(g_camera.mutex);
		m_camera_info = g_camera.info;

		if (m_camera_info.buffer.addr() != addr && m_camera_info.pbuf[0].addr() != addr && m_camera_info.pbuf[1].addr() != addr)
		{
			cellGem.error("gem_tracker: unexpected image address: addr=0x%x, expected one of: 0x%x, 0x%x, 0x%x", addr, m_camera_info.buffer.addr(), m_camera_info.pbuf[0].addr(), m_camera_info.pbuf[1].addr());
			return false;
		}

		// Copy image data for further processing

		const auto& [width, height] = get_video_resolution(m_camera_info);
		const u32 expected_size = get_buffer_size_by_format(m_camera_info.format, width, height);

		if (!m_camera_info.bytesize || static_cast<u32>(m_camera_info.bytesize) != expected_size)
		{
			cellGem.error("gem_tracker: unexpected image size: size=%d, expected=%d", m_camera_info.bytesize, expected_size);
			return false;
		}

		if (!m_camera_info.bytesize)
		{
			cellGem.error("gem_tracker: unexpected image size: %d", m_camera_info.bytesize);
			return false;
		}

		m_tracker.set_image_data(m_camera_info.buffer.get_ptr(), m_camera_info.bytesize, m_camera_info.width, m_camera_info.height, m_camera_info.format);
		return true;
	}

	bool hue_is_trackable(u32 hue)
	{
		if (g_cfg.io.move != move_handler::real)
		{
			return true; // potentially true if less than 20 pixels have the hue
		}

		return hue < m_hues.size() && m_hues[hue] < 20; // potentially true if less than 20 pixels have the hue
	}

	ps_move_info get_info(u32 gem_num)
	{
		std::lock_guard lock(mutex);
		return ::at32(m_info, gem_num);
	}

	gem_tracker& operator=(thread_state)
	{
		wake_up();
		return *this;
	}

	void operator()()
	{
		if (g_cfg.io.move != move_handler::real)
		{
			return;
		}

		if (!g_cfg_move.load())
		{
			cellGem.notice("Could not load PS Move config. Using defaults.");
		}

		auto& gem = g_fxo->get<gem_config>();

		while (thread_ctrl::state() != thread_state::aborting)
		{
			// Check if we have a new frame
			if (!m_wake_up)
			{
				m_wake_up.wait(0);
				m_wake_up.release(0);

				if (thread_ctrl::state() == thread_state::aborting)
				{
					break;
				}
			}

			m_busy.release(true);

			// Update PS Move LED colors
			{
				std::lock_guard lock(pad::g_pad_mutex);
				const auto handler = pad::get_current_handler();
				auto& handlers = handler->get_handlers();
				if (auto it = handlers.find(pad_handler::move); it != handlers.end() && it->second)
				{
					for (auto& binding : it->second->bindings())
					{
						if (!binding.device) continue;

						// last 4 out of 7 ports (6,5,4,3). index starts at 0
						const s32 gem_num = std::abs(binding.device->player_id - CELL_PAD_MAX_PORT_NUM) - 1;

						if (gem_num < 0 || gem_num >= CELL_GEM_MAX_NUM) continue;

						binding.device->color_override_active = true;

						if (g_cfg.io.allow_move_hue_set_by_game)
						{
							const auto& controller = gem.controllers[gem_num];
							binding.device->color_override.r = static_cast<u8>(std::clamp(controller.sphere_rgb.r * 255.0f, 0.0f, 255.0f));
							binding.device->color_override.g = static_cast<u8>(std::clamp(controller.sphere_rgb.g * 255.0f, 0.0f, 255.0f));
							binding.device->color_override.b = static_cast<u8>(std::clamp(controller.sphere_rgb.b * 255.0f, 0.0f, 255.0f));
						}
						else
						{
							const cfg_ps_move* config = ::at32(g_cfg_move.move, gem_num);
							binding.device->color_override.r = config->r.get();
							binding.device->color_override.g = config->g.get();
							binding.device->color_override.b = config->b.get();
						}
					}
				}
			}

			// Update tracker config
			for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
			{
				const auto& controller = gem.controllers[gem_num];
				const cfg_ps_move* config = g_cfg_move.move[gem_num];

				m_tracker.set_active(gem_num, controller.enabled_tracking && controller.status == CELL_GEM_STATUS_READY);
				m_tracker.set_hue(gem_num, g_cfg.io.allow_move_hue_set_by_game ? controller.hue : config->hue);
				m_tracker.set_hue_threshold(gem_num, config->hue_threshold);
				m_tracker.set_saturation_threshold(gem_num, config->saturation_threshold);
			}

			m_tracker.set_min_radius(static_cast<f32>(g_cfg_move.min_radius) / 100.0f);
			m_tracker.set_max_radius(static_cast<f32>(g_cfg_move.max_radius) / 100.0f);

			// Process camera image
			m_tracker.process_image();

			// Update cellGem with results
			{
				std::lock_guard lock(mutex);
				m_hues = m_tracker.hues();
				m_info = m_tracker.info();

				for (u32 gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
				{
					const ps_move_info& info = m_info[gem_num];
					auto& controller = gem.controllers[gem_num];
					controller.radius_valid = info.valid;

					if (info.valid)
					{
						// Only set new radius and distance if the radius is valid
						controller.radius = info.radius;
						controller.distance_mm = info.distance_mm;
					}
				}
			}

			// Notify that we are finished with this frame
			m_done.release(1);
			m_done.notify_one();

			m_busy.release(false);
		}
	}

	static constexpr auto thread_name = "GemUpdateThread"sv;

	shared_mutex mutex;

private:
	atomic_t<u32> m_wake_up = 0;
	atomic_t<u32> m_done = 1;
	atomic_t<bool> m_busy = false;
	ps_move_tracker<false> m_tracker{};
	CellCameraInfoEx m_camera_info{};
	std::array<u32, 360> m_hues{};
	std::array<ps_move_info, CELL_GEM_MAX_NUM> m_info{};
};

/**
 * \brief Verifies that a Move controller id is valid
 * \param gem_num Move controler ID to verify
 * \return True if the ID is valid, false otherwise
 */
static bool check_gem_num(u32 gem_num)
{
	return gem_num < CELL_GEM_MAX_NUM;
}

static inline void draw_overlay_cursor(u32 gem_num, const gem_config::gem_controller&, s32 x_pos, s32 y_pos, s32 x_max, s32 y_max)
{
	const s16 x = static_cast<s16>(x_pos / (x_max / static_cast<f32>(rsx::overlays::overlay::virtual_width)));
	const s16 y = static_cast<s16>(y_pos / (y_max / static_cast<f32>(rsx::overlays::overlay::virtual_height)));

	// Note: We shouldn't use sphere_rgb here. The game will set it to black in many cases.
	const gem_config_data::gem_color& rgb = gem_config_data::gem_color::get_default_color(gem_num);
	const color4f color = { rgb.r, rgb.g, rgb.b, 0.85f };

	rsx::overlays::set_cursor(rsx::overlays::cursor_offset::cell_gem + gem_num, x, y, color, 2'000'000, false);
}

static inline void pos_to_gem_image_state(u32 gem_num, const gem_config::gem_controller& controller, vm::ptr<CellGemImageState>& gem_image_state, s32 x_pos, s32 y_pos, s32 x_max, s32 y_max)
{
	const auto& shared_data = g_fxo->get<gem_camera_shared>();

	if (x_max <= 0) x_max = shared_data.width;
	if (y_max <= 0) y_max = shared_data.height;

	const f32 scaling_width = x_max / static_cast<f32>(shared_data.width);
	const f32 scaling_height = y_max / static_cast<f32>(shared_data.height);
	const f32 mmPerPixel = CELL_GEM_SPHERE_RADIUS_MM / controller.radius;

	// Image coordinates in pixels
	const f32 image_x = static_cast<f32>(x_pos) / scaling_width;
	const f32 image_y = static_cast<f32>(y_pos) / scaling_height;

	// Centered image coordinates in pixels
	const f32 centered_x = image_x - (shared_data.width / 2.f);
	const f32 centered_y = (shared_data.height / 2.f) - image_y; // Image coordinates increase downwards, so we have to invert this

	// Camera coordinates in mm (centered, so it's the same as world coordinates)
	const f32 camera_x = centered_x * mmPerPixel;
	const f32 camera_y = centered_y * mmPerPixel;

	// Image coordinates in pixels
	gem_image_state->u = image_x;
	gem_image_state->v = image_y;

	// Projected camera coordinates in mm
	gem_image_state->projectionx = camera_x / controller.distance_mm;
	gem_image_state->projectiony = camera_y / controller.distance_mm;

	if (g_cfg.io.show_move_cursor)
	{
		draw_overlay_cursor(gem_num, controller, x_pos, y_pos, x_max, y_max);
	}

	if (g_cfg.io.paint_move_spheres)
	{
		::at32(gem::positions, gem_num).set_position(image_x, image_y);
	}
}

static inline void pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, vm::ptr<CellGemState>& gem_state, s32 x_pos, s32 y_pos, s32 x_max, s32 y_max, const ps_move_data& move_data)
{
	const auto& shared_data = g_fxo->get<gem_camera_shared>();

	if (x_max <= 0) x_max = shared_data.width;
	if (y_max <= 0) y_max = shared_data.height;

	const f32 scaling_width = x_max / static_cast<f32>(shared_data.width);
	const f32 scaling_height = y_max / static_cast<f32>(shared_data.height);
	const f32 mmPerPixel = CELL_GEM_SPHERE_RADIUS_MM / controller.radius;

	// Image coordinates in pixels
	const f32 image_x = static_cast<f32>(x_pos) / scaling_width;
	const f32 image_y = static_cast<f32>(y_pos) / scaling_height;

	// Half of the camera image
	const f32 half_width = shared_data.width / 2.f;
	const f32 half_height = shared_data.height / 2.f;

	// Centered image coordinates in pixels
	const f32 centered_x = image_x - half_width;
	const f32 centered_y = half_height - image_y; // Image coordinates increase downwards, so we have to invert this

	// Camera coordinates in mm (centered, so it's the same as world coordinates)
	const f32 camera_x = centered_x * mmPerPixel;
	const f32 camera_y = centered_y * mmPerPixel;

	// World coordinates in mm
	gem_state->pos[0] = camera_x;
	gem_state->pos[1] = camera_y;
	gem_state->pos[2] = controller.distance_mm;
	gem_state->pos[3] = 0.f;

	// TODO: calculate handle position based on our world coordinate and the angles
	gem_state->handle_pos[0] = camera_x;
	gem_state->handle_pos[1] = camera_y;
	gem_state->handle_pos[2] = controller.distance_mm + 10.0f;
	gem_state->handle_pos[3] = 0.f;

	// Calculate orientation
	if (g_cfg.io.move == move_handler::real || (g_cfg.io.move == move_handler::fake && move_data.orientation_enabled))
	{
		gem_state->quat[0] = move_data.quaternion[0]; // x
		gem_state->quat[1] = move_data.quaternion[1]; // y
		gem_state->quat[2] = move_data.quaternion[2]; // z
		gem_state->quat[3] = move_data.quaternion[3]; // w
	}
	else
	{
		const f32 max_angle_per_side_h = g_cfg.io.fake_move_rotation_cone_h / 2.0f;
		const f32 max_angle_per_side_v = g_cfg.io.fake_move_rotation_cone_v / 2.0f;
		const f32 roll = -PadHandlerBase::degree_to_rad((image_y - half_height) / half_height * max_angle_per_side_v); // This is actually the pitch
		const f32 pitch = -PadHandlerBase::degree_to_rad((image_x - half_width) / half_width * max_angle_per_side_h); // This is actually the yaw
		const f32 yaw = PadHandlerBase::degree_to_rad(0.0f);
		const f32 cr = std::cos(roll * 0.5f);
		const f32 sr = std::sin(roll * 0.5f);
		const f32 cp = std::cos(pitch * 0.5f);
		const f32 sp = std::sin(pitch * 0.5f);
		const f32 cy = std::cos(yaw * 0.5f);
		const f32 sy = std::sin(yaw * 0.5f);

		const f32 q_x = sr * cp * cy - cr * sp * sy;
		const f32 q_y = cr * sp * cy + sr * cp * sy;
		const f32 q_z = cr * cp * sy - sr * sp * cy;
		const f32 q_w = cr * cp * cy + sr * sp * sy;

		gem_state->quat[0] = q_x;
		gem_state->quat[1] = q_y;
		gem_state->quat[2] = q_z;
		gem_state->quat[3] = q_w;
	}

	if (g_cfg.io.show_move_cursor)
	{
		draw_overlay_cursor(gem_num, controller, x_pos, y_pos, x_max, y_max);
	}

	if (g_cfg.io.paint_move_spheres)
	{
		::at32(gem::positions, gem_num).set_position(image_x, image_y);
	}
}

extern bool is_input_allowed();

/**
 * \brief Maps Move controller data (digital buttons, and analog Trigger data) to DS3 pad input.
 *        Unavoidably buttons conflict with DS3 mappings, which is problematic for some games.
 * \param gem_num gem index to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger. Currently mapped to R2.
 * \return true on success, false if controller is disconnected
 */
static void ds3_input_to_pad(const u32 gem_num, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed())
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_current_handler();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		return;
	}

	const auto handle_input = [&](gem_btn btn, u16 value, bool pressed)
	{
		if (!pressed)
			return;

		switch (btn)
		{
		case gem_btn::start:
			digital_buttons |= CELL_GEM_CTRL_START;
			break;
		case gem_btn::select:
			digital_buttons |= CELL_GEM_CTRL_SELECT;
			break;
		case gem_btn::square:
			digital_buttons |= CELL_GEM_CTRL_SQUARE;
			break;
		case gem_btn::cross:
			digital_buttons |= CELL_GEM_CTRL_CROSS;
			break;
		case gem_btn::circle:
			digital_buttons |= CELL_GEM_CTRL_CIRCLE;
			break;
		case gem_btn::triangle:
			digital_buttons |= CELL_GEM_CTRL_TRIANGLE;
			break;
		case gem_btn::move:
			digital_buttons |= CELL_GEM_CTRL_MOVE;
			break;
		case gem_btn::t:
			digital_buttons |= CELL_GEM_CTRL_T;
			analog_t = std::max<u16>(analog_t, value);
			break;
		case gem_btn::x_axis:
		case gem_btn::y_axis:
		case gem_btn::count:
			break;
		}
	};

	if (g_cfg.io.move == move_handler::real)
	{
		::at32(g_cfg_gem_real.players, gem_num)->handle_input(pad, true, handle_input);
	}
	else
	{
		::at32(g_cfg_gem_fake.players, gem_num)->handle_input(pad, true, handle_input);
	}
}

constexpr u16 ds3_max_x = 255;
constexpr u16 ds3_max_y = 255;

static inline void ds3_get_stick_values(u32 gem_num, const std::shared_ptr<Pad>& pad, s32& x_pos, s32& y_pos)
{
	x_pos = 0;
	y_pos = 0;

	const auto& cfg = ::at32(g_cfg_gem_fake.players, gem_num);
	cfg->handle_input(pad, true, [&](gem_btn btn, u16 value, bool pressed)
		{
			if (!pressed)
				return;

			switch (btn)
			{
			case gem_btn::x_axis:
				x_pos = value;
				break;
			case gem_btn::y_axis:
				y_pos = value;
				break;
			default:
				break;
			}
		});
}

template <typename T>
static void ds3_pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed())
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_current_handler();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		return;
	}

	s32 ds3_pos_x, ds3_pos_y;
	ds3_get_stick_values(gem_num, pad, ds3_pos_x, ds3_pos_y);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		pos_to_gem_state(gem_num, controller, gem_state, ds3_pos_x, ds3_pos_y, ds3_max_x, ds3_max_y, pad->move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_num, controller, gem_state, ds3_pos_x, ds3_pos_y, ds3_max_x, ds3_max_y);
	}
}

template <typename T>
static void ps_move_pos_to_gem_state(u32 gem_num, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed())
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_current_handler();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (pad->m_pad_handler != pad_handler::move || !(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		return;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	const ps_move_info info = tracker.get_info(gem_num);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		gem_state->temperature = pad->move_data.temperature;
		gem_state->accel[0] = pad->move_data.accelerometer_x * 1000; // linear velocity in mm/s²
		gem_state->accel[1] = pad->move_data.accelerometer_y * 1000; // linear velocity in mm/s²
		gem_state->accel[2] = pad->move_data.accelerometer_z * 1000; // linear velocity in mm/s²

		pos_to_gem_state(gem_num, controller, gem_state, info.x_pos, info.y_pos, info.x_max, info.y_max, pad->move_data);
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_num, controller, gem_state, info.x_pos, info.y_pos, info.x_max, info.y_max);
	}
}

/**
 * \brief Maps external Move controller data to DS3 input. (This can be input from any physical pad, not just the DS3)
 *        Implementation detail: CellGemExtPortData's digital/analog fields map the same way as
 *        libPad, so no translation is needed.
 * \param gem_num gem index to use
 * \param ext External data to modify
 * \return true on success, false if controller is disconnected
 */
static void ds3_input_to_ext(u32 gem_num, gem_config::gem_controller& controller, CellGemExtPortData& ext)
{
	ext = {};

	if (!is_input_allowed())
	{
		return;
	}

	std::lock_guard lock(pad::g_pad_mutex);

	const auto handler = pad::get_current_handler();
	const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

	if (!(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
	{
		return;
	}

	const auto& move_data = pad->move_data;

	controller.ext_status = move_data.external_device_connected ? CELL_GEM_EXT_CONNECTED : 0; // TODO: | CELL_GEM_EXT_EXT0 | CELL_GEM_EXT_EXT1
	controller.ext_id = move_data.external_device_connected ? move_data.external_device_id : 0;

	ext.status = controller.ext_status;

	for (const AnalogStick& stick : pad->m_sticks)
	{
		switch (stick.m_offset)
		{
		case CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X: ext.analog_left_x = stick.m_value; break;
		case CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y: ext.analog_left_y = stick.m_value; break;
		case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X: ext.analog_right_x = stick.m_value; break;
		case CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y: ext.analog_right_y = stick.m_value; break;
		default: break;
		}
	}

	for (const Button& button : pad->m_buttons)
	{
		if (!button.m_pressed)
			continue;

		switch (button.m_offset)
		{
		case CELL_PAD_BTN_OFFSET_DIGITAL1: ext.digital1 |= button.m_outKeyCode; break;
		case CELL_PAD_BTN_OFFSET_DIGITAL2: ext.digital2 |= button.m_outKeyCode; break;
		default: break;
		}
	}

	if (!move_data.external_device_connected)
	{
		return;
	}

	// The sharpshooter only sets the custom bytes as follows:
	// custom[0] (0x01): Firing mode selector is in position 1.
	// custom[0] (0x02): Firing mode selector is in position 2.
	// custom[0] (0x04): Firing mode selector is in position 3.
	// custom[0] (0x40): T button trigger is pressed.
	// custom[0] (0x80): RL reload button is pressed.

	// The racing wheel sets the custom bytes as follows:
	// custom[0] 0-255:  Throttle
	// custom[1] 0-255:  L2
	// custom[2] 0-255:  R2
	// custom[3] (0x01): Left paddle
	// custom[3] (0x02): Right paddle

	std::memcpy(ext.custom, move_data.external_device_data.data(), 5);
}

/**
 * \brief Maps Move controller data (digital buttons, and analog Trigger data) to mouse input.
 * \param mouse_no Mouse index number to use
 * \param digital_buttons Bitmask filled with CELL_GEM_CTRL_* values
 * \param analog_t Analog value of Move's Trigger.
 * \return true on success, false if mouse_no is invalid
 */
static bool mouse_input_to_pad(u32 mouse_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed())
	{
		return false;
	}

	auto& handler = g_fxo->get<MouseHandlerBase>();

	std::scoped_lock lock(handler.mutex);

	// Make sure that the mouse handler is initialized
	handler.Init(std::min<u32>(g_fxo->get<gem_config>().attribute.max_connect, CELL_GEM_MAX_NUM));

	if (mouse_no >= handler.GetMice().size())
	{
		return false;
	}

	std::set<MouseButtonCodes> pressed_buttons;
	const Mouse& mouse_data = ::at32(handler.GetMice(), mouse_no);
	const auto is_pressed = [&mouse_data, &pressed_buttons](MouseButtonCodes button) -> bool
	{
		// Only allow each button to be used for one action unless it's the combo button.
		return (mouse_data.buttons & button) && (button == (CELL_MOUSE_BUTTON_3 + 0u/*fix warning*/) || pressed_buttons.insert(button).second);
	};

	digital_buttons = 0;

	if ((is_pressed(CELL_MOUSE_BUTTON_3) && is_pressed(CELL_MOUSE_BUTTON_1)) || is_pressed(CELL_MOUSE_BUTTON_6))
		digital_buttons |= CELL_GEM_CTRL_SELECT;

	if ((is_pressed(CELL_MOUSE_BUTTON_3) && is_pressed(CELL_MOUSE_BUTTON_2)) || is_pressed(CELL_MOUSE_BUTTON_7))
		digital_buttons |= CELL_GEM_CTRL_START;

	if ((is_pressed(CELL_MOUSE_BUTTON_3) && is_pressed(CELL_MOUSE_BUTTON_4)) || is_pressed(CELL_MOUSE_BUTTON_8))
		digital_buttons |= CELL_GEM_CTRL_TRIANGLE;

	if (is_pressed(CELL_MOUSE_BUTTON_3) && is_pressed(CELL_MOUSE_BUTTON_5))
		digital_buttons |= CELL_GEM_CTRL_SQUARE;

	if (is_pressed(CELL_MOUSE_BUTTON_1))
		digital_buttons |= CELL_GEM_CTRL_T;

	if (is_pressed(CELL_MOUSE_BUTTON_2))
		digital_buttons |= CELL_GEM_CTRL_MOVE;

	if (is_pressed(CELL_MOUSE_BUTTON_4))
		digital_buttons |= CELL_GEM_CTRL_CIRCLE;

	if (is_pressed(CELL_MOUSE_BUTTON_5))
		digital_buttons |= CELL_GEM_CTRL_CROSS;

	analog_t = (mouse_data.buttons & CELL_MOUSE_BUTTON_1) ? 0xFFFF : 0;

	return true;
}

template <typename T>
static void mouse_pos_to_gem_state(u32 mouse_no, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed())
	{
		return;
	}

	auto& handler = g_fxo->get<MouseHandlerBase>();

	std::scoped_lock lock(handler.mutex);

	// Make sure that the mouse handler is initialized
	handler.Init(std::min<u32>(g_fxo->get<gem_config>().attribute.max_connect, CELL_GEM_MAX_NUM));

	if (mouse_no >= handler.GetMice().size())
	{
		return;
	}

	const auto& mouse = ::at32(handler.GetMice(), mouse_no);

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		pos_to_gem_state(mouse_no, controller, gem_state, mouse.x_pos, mouse.y_pos, mouse.x_max, mouse.y_max, {});
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(mouse_no, controller, gem_state, mouse.x_pos, mouse.y_pos, mouse.x_max, mouse.y_max);
	}
}

#ifdef HAVE_LIBEVDEV
static bool gun_input_to_pad(u32 gem_no, be_t<u16>& digital_buttons, be_t<u16>& analog_t)
{
	digital_buttons = 0;
	analog_t = 0;

	if (!is_input_allowed())
		return false;

	gun_thread& gun = g_fxo->get<gun_thread>();
	std::scoped_lock lock(gun.handler.mutex);

	if (gun.handler.get_button(gem_no, gun_button::btn_left) == 1)
		digital_buttons |= CELL_GEM_CTRL_T;

	if (gun.handler.get_button(gem_no, gun_button::btn_right) == 1)
		digital_buttons |= CELL_GEM_CTRL_MOVE;

	if (gun.handler.get_button(gem_no, gun_button::btn_middle) == 1)
		digital_buttons |= CELL_GEM_CTRL_START;

	if (gun.handler.get_button(gem_no, gun_button::btn_1) == 1)
		digital_buttons |= CELL_GEM_CTRL_CROSS;

	if (gun.handler.get_button(gem_no, gun_button::btn_2) == 1)
		digital_buttons |= CELL_GEM_CTRL_CIRCLE;

	if (gun.handler.get_button(gem_no, gun_button::btn_3) == 1)
		digital_buttons |= CELL_GEM_CTRL_SELECT;

	if (gun.handler.get_button(gem_no, gun_button::btn_5) == 1)
		digital_buttons |= CELL_GEM_CTRL_TRIANGLE;

	if (gun.handler.get_button(gem_no, gun_button::btn_6) == 1)
		digital_buttons |= CELL_GEM_CTRL_SQUARE;

	analog_t = gun.handler.get_button(gem_no, gun_button::btn_left) ? 0xFFFF : 0;

	return true;
}

template <typename T>
static void gun_pos_to_gem_state(u32 gem_no, gem_config::gem_controller& controller, T& gem_state)
{
	if (!gem_state || !is_input_allowed())
		return;

	int x_pos, y_pos, x_max, y_max;
	{
		gun_thread& gun = g_fxo->get<gun_thread>();
		std::scoped_lock lock(gun.handler.mutex);

		x_pos = gun.handler.get_axis_x(gem_no);
		y_pos = gun.handler.get_axis_y(gem_no);
		x_max = gun.handler.get_axis_x_max(gem_no);
		y_max = gun.handler.get_axis_y_max(gem_no);
	}

	if constexpr (std::is_same_v<T, vm::ptr<CellGemState>>)
	{
		pos_to_gem_state(gem_no, controller, gem_state, x_pos, y_pos, x_max, y_max, {});
	}
	else if constexpr (std::is_same_v<T, vm::ptr<CellGemImageState>>)
	{
		pos_to_gem_image_state(gem_no, controller, gem_state, x_pos, y_pos, x_max, y_max);
	}
}
#endif

// *********************
// * cellGem functions *
// *********************

error_code cellGemCalibrate(u32 gem_num)
{
	cellGem.todo("cellGemCalibrate(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& controller = gem.controllers[gem_num];

	if (controller.is_calibrating)
	{
		return CELL_EBUSY;
	}

	controller.is_calibrating = true;
	controller.calibration_start_us = get_guest_system_time();

	return CELL_OK;
}

error_code cellGemClearStatusFlags(u32 gem_num, u64 mask)
{
	cellGem.todo("cellGemClearStatusFlags(gem_num=%d, mask=0x%x)", gem_num, mask);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].calibration_status_flags &= ~mask;

	return CELL_OK;
}

error_code cellGemConvertVideoFinish()
{
	cellGem.warning("cellGemConvertVideoFinish()");

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!gem.video_conversion_in_progress)
	{
		return CELL_GEM_ERROR_CONVERT_NOT_STARTED;
	}

	while (gem.video_conversion_in_progress && !Emu.IsStopped())
	{
		thread_ctrl::wait_for(100);
	}

	return CELL_OK;
}

error_code cellGemConvertVideoStart(vm::cptr<void> video_frame)
{
	cellGem.warning("cellGemConvertVideoStart(video_frame=*0x%x)", video_frame);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!video_frame)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!video_frame.aligned(128))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	if (gem.video_conversion_in_progress)
	{
		return CELL_GEM_ERROR_CONVERT_NOT_FINISHED;
	}

	const auto& shared_data = g_fxo->get<gem_camera_shared>();
	gem.video_data_in.resize(shared_data.size);
	std::memcpy(gem.video_data_in.data(), video_frame.get_ptr(), gem.video_data_in.size());
	gem.video_conversion_in_progress = true;

	return CELL_OK;
}

error_code cellGemEnableCameraPitchAngleCorrection(u32 enable_flag)
{
	cellGem.todo("cellGemEnableCameraPitchAngleCorrection(enable_flag=%d)", enable_flag);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	gem.enable_pitch_correction = !!enable_flag;

	return CELL_OK;
}

error_code cellGemEnableMagnetometer(u32 gem_num, u32 enable)
{
	cellGem.todo("cellGemEnableMagnetometer(gem_num=%d, enable=0x%x)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	auto& controller = gem.controllers[gem_num];

	// NOTE: RE doesn't show this check but it is mentioned in the docs, so I'll leave it here for now.
	//if (!controller.calibrated_magnetometer)
	//{
	//	return CELL_GEM_NOT_CALIBRATED;
	//}

	controller.enabled_magnetometer = !!enable;

	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_current_handler();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad && pad->m_pad_handler == pad_handler::move)
		{
			pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
		}
	}

	return CELL_OK;
}

error_code cellGemEnableMagnetometer2(u32 gem_num, u32 enable)
{
	cellGem.trace("cellGemEnableMagnetometer2(gem_num=%d, enable=0x%x)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	auto& controller = gem.controllers[gem_num];

	if (!controller.calibrated_magnetometer)
	{
		return CELL_GEM_NOT_CALIBRATED;
	}

	controller.enabled_magnetometer = !!enable;

	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_current_handler();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad && pad->m_pad_handler == pad_handler::move)
		{
			pad->move_data.magnetometer_enabled = controller.enabled_magnetometer;
		}
	}

	return CELL_OK;
}

error_code cellGemEnd(ppu_thread& ppu)
{
	cellGem.warning("cellGemEnd()");

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (gem.state.compare_and_swap_test(1, 0))
	{
		if (u32 addr = std::exchange(gem.memory_ptr, 0))
		{
			sys_memory_free(ppu, addr);
		}

		return CELL_OK;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	tracker.wait_for_result();

	gem.updating = false;

	return CELL_GEM_ERROR_UNINITIALIZED;
}

error_code cellGemFilterState(u32 gem_num, u32 enable)
{
	cellGem.warning("cellGemFilterState(gem_num=%d, enable=%d)", gem_num, enable);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].enabled_filtering = !!enable;

	return CELL_OK;
}

error_code cellGemForceRGB(u32 gem_num, f32 r, f32 g, f32 b)
{
	cellGem.todo("cellGemForceRGB(gem_num=%d, r=%f, g=%f, b=%f)", gem_num, r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO: Adjust brightness
	//if (const f32 sum = r + g + b; sum > 2.f)
	//{
	//	color = color * (2.f / sum)
	//}

	auto& controller = gem.controllers[gem_num];

	controller.sphere_rgb = gem_config::gem_color(r, g, b);
	controller.enabled_tracking = false;

	const auto [h, s, v] = ps_move_tracker<false>::rgb_to_hsv(r, g, b);
	controller.hue = h;

	return CELL_OK;
}

error_code cellGemGetAccelerometerPositionInDevice(u32 gem_num, vm::ptr<f32> pos)
{
	cellGem.todo("cellGemGetAccelerometerPositionInDevice(gem_num=%d, pos=*0x%x)", gem_num, pos);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !pos)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO
	pos[0] = 0.0f;
	pos[1] = 0.0f;
	pos[2] = 0.0f;
	pos[3] = 0.0f;

	return CELL_OK;
}

error_code cellGemGetAllTrackableHues(vm::ptr<u8> hues)
{
	cellGem.todo("cellGemGetAllTrackableHues(hues=*0x%x)");

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	std::lock_guard lock(tracker.mutex);

	for (u32 hue = 0; hue < 360; hue++)
	{
		hues[hue] = tracker.hue_is_trackable(hue);
	}

	return CELL_OK;
}

error_code cellGemGetCameraState(vm::ptr<CellGemCameraState> camera_state)
{
	cellGem.todo("cellGemGetCameraState(camera_state=0x%x)", camera_state);

	[[maybe_unused]] auto& gem = g_fxo->get<gem_config>();

	if (!camera_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO: use correct camera settings
	camera_state->exposure = 0;
	camera_state->exposure_time = 1.0f / 60.0f;
	camera_state->gain = 1.0f;
	camera_state->pitch_angle = 0.0f;
	camera_state->pitch_angle_estimate = 0.0f;

	return CELL_OK;
}

error_code cellGemGetEnvironmentLightingColor(vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemGetEnvironmentLightingColor(r=*0x%x, g=*0x%x, b=*0x%x)", r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// default to 128
	*r = 128;
	*g = 128;
	*b = 128;

	// NOTE: RE doesn't show this check but it is mentioned in the docs, so I'll leave it here for now.
	//if (!gem.controllers[gem_num].calibrated_magnetometer)
	//{
	//	return CELL_GEM_ERROR_LIGHTING_NOT_CALIBRATED; // This error doesn't really seem to be a real thing.
	//}

	return CELL_OK;
}

error_code cellGemGetHuePixels(vm::cptr<void> camera_frame, u32 hue, vm::ptr<u8> pixels)
{
	cellGem.todo("cellGemGetHuePixels(camera_frame=*0x%x, hue=%d, pixels=*0x%x)", camera_frame, hue, pixels);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!camera_frame || !pixels || hue > 359)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::memset(pixels.get_ptr(), 0, 640 * 480 * sizeof(u8));

	if (g_cfg.io.move == move_handler::real)
	{
		// TODO: get pixels from tracker
	}

	return CELL_OK;
}

error_code cellGemGetImageState(u32 gem_num, vm::ptr<CellGemImageState> gem_image_state)
{
	cellGem.warning("cellGemGetImageState(gem_num=%d, image_state=&0x%x)", gem_num, gem_image_state);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !gem_image_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*gem_image_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		auto& shared_data = g_fxo->get<gem_camera_shared>();
		auto& controller = gem.controllers[gem_num];

		gem_image_state->frame_timestamp = shared_data.frame_timestamp_us.load();
		gem_image_state->timestamp = gem_image_state->frame_timestamp + 10;
		gem_image_state->r = controller.radius; // Radius in camera pixels
		gem_image_state->distance = controller.distance_mm;
		gem_image_state->visible = gem.is_controller_ready(gem_num);
		gem_image_state->r_valid = controller.radius_valid;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
			ps_move_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
		case move_handler::fake:
			ds3_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_pos_to_gem_state(gem_num, controller, gem_image_state);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}
	}

	return CELL_OK;
}

error_code cellGemGetInertialState(u32 gem_num, u32 state_flag, u64 timestamp, vm::ptr<CellGemInertialState> inertial_state)
{
	cellGem.warning("cellGemGetInertialState(gem_num=%d, state_flag=%d, timestamp=0x%x, inertial_state=0x%x)", gem_num, state_flag, timestamp, inertial_state);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !inertial_state || !gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (false) // TODO
	{
		return CELL_GEM_TIME_OUT_OF_RANGE;
	}

	*inertial_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		ds3_input_to_ext(gem_num, gem.controllers[gem_num], inertial_state->ext);

		inertial_state->timestamp = (get_guest_system_time() - gem.start_timestamp_us);
		inertial_state->counter = gem.inertial_counter++;
		inertial_state->accelerometer[0] = 10; // Current gravity in m/s²

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		case move_handler::fake:
		{
			// Get temperature and sensor data
			{
				std::lock_guard lock(pad::g_pad_mutex);

				const auto handler = pad::get_current_handler();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

				if (pad && (pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
				{
					inertial_state->temperature = pad->move_data.temperature;
					inertial_state->accelerometer[0] = pad->move_data.accelerometer_x;
					inertial_state->accelerometer[1] = pad->move_data.accelerometer_y;
					inertial_state->accelerometer[2] = pad->move_data.accelerometer_z;
					inertial_state->gyro[0] = pad->move_data.gyro_x;
					inertial_state->gyro[1] = pad->move_data.gyro_y;
					inertial_state->gyro[2] = pad->move_data.gyro_z;
				}
			}

			ds3_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			break;
		}
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_input_to_pad(gem_num, inertial_state->pad.digitalbuttons, inertial_state->pad.analog_T);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}
	}

	return CELL_OK;
}

error_code cellGemGetInfo(vm::ptr<CellGemInfo> info)
{
	cellGem.trace("cellGemGetInfo(info=*0x%x)", info);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!info)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	switch (g_cfg.io.move)
	{
	case move_handler::real:
	case move_handler::fake:
	{
		gem.connected_controllers = 0;

		std::lock_guard lock(pad::g_pad_mutex);
		const auto handler = pad::get_current_handler();

		for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
		{
			const auto& pad = ::at32(handler->GetPads(), pad_num(i));
			const bool connected = (pad && (pad->m_port_status & CELL_PAD_STATUS_CONNECTED) && i < gem.attribute.max_connect);
			const bool is_real_move = g_cfg.io.move != move_handler::real || pad->m_pad_handler == pad_handler::move;

			if (connected && is_real_move)
			{
				gem.connected_controllers++;
				gem.controllers[i].status = CELL_GEM_STATUS_READY;
				gem.controllers[i].port = port_num(i);
			}
			else
			{
				gem.controllers[i].status = CELL_GEM_STATUS_DISCONNECTED;
				gem.controllers[i].port = 0;
			}
		}
		break;
	}
	case move_handler::raw_mouse:
	{
		gem.connected_controllers = 0;

		auto& handler = g_fxo->get<MouseHandlerBase>();
		std::lock_guard mouse_lock(handler.mutex);

		const MouseInfo& info = handler.GetInfo();

		for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
		{
			const bool connected = i < gem.attribute.max_connect && info.status[i] == CELL_MOUSE_STATUS_CONNECTED;

			if (connected)
			{
				gem.connected_controllers++;
				gem.controllers[i].status = CELL_GEM_STATUS_READY;
				gem.controllers[i].port = port_num(i);
			}
			else
			{
				gem.controllers[i].status = CELL_GEM_STATUS_DISCONNECTED;
				gem.controllers[i].port = 0;
			}
		}
		break;
	}
	default:
	{
		break;
	}
	}

	info->max_connect = gem.attribute.max_connect;
	info->now_connect = gem.connected_controllers;

	for (int i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		info->status[i] = gem.controllers[i].status;
		info->port[i] = gem.controllers[i].port;
	}

	return CELL_OK;
}

u32 GemGetMemorySize(s32 max_connect)
{
	return max_connect <= 2 ? 0x120000 : 0x140000;
}

error_code cellGemGetMemorySize(s32 max_connect)
{
	cellGem.warning("cellGemGetMemorySize(max_connect=%d)", max_connect);

	if (max_connect > CELL_GEM_MAX_NUM || max_connect <= 0)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	return not_an_error(GemGetMemorySize(max_connect));
}

error_code cellGemGetRGB(u32 gem_num, vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.todo("cellGemGetRGB(gem_num=%d, r=*0x%x, g=*0x%x, b=*0x%x)", gem_num, r, g, b);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const gem_config_data::gem_color& sphere_color = gem.controllers[gem_num].sphere_rgb;
	*r = sphere_color.r;
	*g = sphere_color.g;
	*b = sphere_color.b;

	return CELL_OK;
}

error_code cellGemGetRumble(u32 gem_num, vm::ptr<u8> rumble)
{
	cellGem.todo("cellGemGetRumble(gem_num=%d, rumble=*0x%x)", gem_num, rumble);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !rumble)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*rumble = gem.controllers[gem_num].rumble;

	return CELL_OK;
}

error_code cellGemGetState(u32 gem_num, u32 flag, u64 time_parameter, vm::ptr<CellGemState> gem_state)
{
	cellGem.warning("cellGemGetState(gem_num=%d, flag=0x%x, time=0x%llx, gem_state=*0x%x)", gem_num, flag, time_parameter, gem_state);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || flag > CELL_GEM_STATE_FLAG_TIMESTAMP || !gem_state)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return not_an_error(CELL_GEM_NOT_CONNECTED);
	}

	// TODO: Get the gem state at the specified time
	//if (flag == CELL_GEM_STATE_FLAG_CURRENT_TIME)
	//{
	//	// now + time_parameter (time_parameter in microseconds). Positive values actually allow predictions for the future state.
	//}
	//else if (flag == CELL_GEM_STATE_FLAG_LATEST_IMAGE_TIME)
	//{
	//	// When the sphere was registered during the last camera frame (time_parameter may also have an impact)
	//}
	//else // CELL_GEM_STATE_FLAG_TIMESTAMP
	//{
	//	// As specified by time_parameter.
	//}

	if (false) // TODO: check if there is data for the specified time_parameter and flag
	{
		return CELL_GEM_TIME_OUT_OF_RANGE;
	}

	auto& controller = gem.controllers[gem_num];

	*gem_state = {};

	if (g_cfg.io.move != move_handler::null)
	{
		ds3_input_to_ext(gem_num, controller, gem_state->ext);

		if (controller.enabled_tracking)
		{
			gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_POSITION_TRACKED;
			gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_VISIBLE;
		}

		gem_state->timestamp = (get_guest_system_time() - gem.start_timestamp_us);
		gem_state->camera_pitch_angle = 0.f;

		switch (g_cfg.io.move)
		{
		case move_handler::real:
		{
			auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
			const ps_move_info info = tracker.get_info(gem_num);

			ds3_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			ps_move_pos_to_gem_state(gem_num, controller, gem_state);

			if (info.valid)
				gem_state->tracking_flags |= CELL_GEM_TRACKING_FLAG_VISIBLE;
			else
				gem_state->tracking_flags &= ~CELL_GEM_TRACKING_FLAG_VISIBLE;

			break;
		}
		case move_handler::fake:
			ds3_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			ds3_pos_to_gem_state(gem_num, controller, gem_state);
			break;
		case move_handler::mouse:
		case move_handler::raw_mouse:
			mouse_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			mouse_pos_to_gem_state(gem_num, controller, gem_state);
			break;
#ifdef HAVE_LIBEVDEV
		case move_handler::gun:
			gun_input_to_pad(gem_num, gem_state->pad.digitalbuttons, gem_state->pad.analog_T);
			gun_pos_to_gem_state(gem_num, controller, gem_state);
			break;
#endif
		case move_handler::null:
			fmt::throw_exception("Unreachable");
		}
	}

	if (false) // TODO: check if we are computing colors
	{
		return CELL_GEM_COMPUTING_AVAILABLE_COLORS;
	}

	if (controller.is_calibrating)
	{
		return CELL_GEM_SPHERE_CALIBRATING;
	}

	if (!controller.calibrated_magnetometer)
	{
		return CELL_GEM_SPHERE_NOT_CALIBRATED;
	}

	if (!controller.hue_set)
	{
		return CELL_GEM_HUE_NOT_SET;
	}

	return CELL_OK;
}

error_code cellGemGetStatusFlags(u32 gem_num, vm::ptr<u64> flags)
{
	cellGem.trace("cellGemGetStatusFlags(gem_num=%d, flags=*0x%x)", gem_num, flags);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !flags)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	*flags = gem.runtime_status_flags | gem.controllers[gem_num].calibration_status_flags;

	return CELL_OK;
}

error_code cellGemGetTrackerHue(u32 gem_num, vm::ptr<u32> hue)
{
	cellGem.warning("cellGemGetTrackerHue(gem_num=%d, hue=*0x%x)", gem_num, hue);

	auto& gem = g_fxo->get<gem_config>();

	reader_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !hue)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const auto& controller = gem.controllers[gem_num];

	if (!controller.enabled_tracking || controller.hue > 359)
	{
		return { CELL_GEM_ERROR_NOT_A_HUE, controller.hue };
	}

	*hue = controller.hue;

	return CELL_OK;
}

error_code cellGemHSVtoRGB(f32 h, f32 s, f32 v, vm::ptr<f32> r, vm::ptr<f32> g, vm::ptr<f32> b)
{
	cellGem.warning("cellGemHSVtoRGB(h=%f, s=%f, v=%f, r=*0x%x, g=*0x%x, b=*0x%x)", h, s, v, r, g, b);

	if (s < 0.0f || s > 1.0f || v < 0.0f || v > 1.0f || !r || !g || !b)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	h = std::clamp(h, 0.0f, 360.0f);

	const f32 c = v * s;
	const f32 x = c * (1.0f - fabs(fmod(h / 60.0f, 2.0f) - 1.0f));
	const f32 m = v - c;

	f32 r_tmp{};
	f32 g_tmp{};
	f32 b_tmp{};

	if (h < 60.0f)
	{
		r_tmp = c;
		g_tmp = x;
	}
	else if (h < 120.0f)
	{
		r_tmp = x;
		g_tmp = c;
	}
	else if (h < 180.0f)
	{
		g_tmp = c;
		b_tmp = x;
	}
	else if (h < 240.0f)
	{
		g_tmp = x;
		b_tmp = c;
	}
	else if (h < 300.0f)
	{
		r_tmp = x;
		b_tmp = c;
	}
	else
	{
		r_tmp = c;
		b_tmp = x;
	}

	*r = (r_tmp + m) * 255.0f;
	*g = (g_tmp + m) * 255.0f;
	*b = (b_tmp + m) * 255.0f;

	return CELL_OK;
}

error_code cellGemInit(ppu_thread& ppu, vm::cptr<CellGemAttribute> attribute)
{
	cellGem.warning("cellGemInit(attribute=*0x%x)", attribute);

	auto& gem = g_fxo->get<gem_config>();

	if (!attribute || !attribute->spurs_addr || !attribute->max_connect || attribute->max_connect > CELL_GEM_MAX_NUM)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	std::scoped_lock lock(gem.mtx);

	if (!gem.state.compare_and_swap_test(0, 1))
	{
		return CELL_GEM_ERROR_ALREADY_INITIALIZED;
	}

	if (!attribute->memory_ptr)
	{
		vm::var<u32> addr(0);

		// Decrease memory stats
		if (sys_memory_allocate(ppu, GemGetMemorySize(attribute->max_connect), SYS_MEMORY_PAGE_SIZE_64K, +addr) != CELL_OK)
		{
			return CELL_GEM_ERROR_RESOURCE_ALLOCATION_FAILED;
		}

		gem.memory_ptr = *addr;
	}
	else
	{
		gem.memory_ptr = 0;
	}

	gem.updating = false;
	gem.camera_frame = 0;
	gem.runtime_status_flags = 0;
	gem.attribute = *attribute;

	for (int gem_num = 0; gem_num < CELL_GEM_MAX_NUM; gem_num++)
	{
		gem.reset_controller(gem_num);
	}

	// TODO: is this correct?
	gem.start_timestamp_us = get_guest_system_time();

	return CELL_OK;
}

error_code cellGemInvalidateCalibration(s32 gem_num)
{
	cellGem.todo("cellGemInvalidateCalibration(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& controller = gem.controllers[gem_num];

	// TODO: does this really stop an ongoing calibration ?
	controller.calibrated_magnetometer = false;
	controller.is_calibrating = false;
	controller.calibration_start_us = 0;
	controller.calibration_status_flags = 0;
	controller.hue_set = false;
	controller.enabled_tracking = false;

	return CELL_OK;
}

s32 cellGemIsTrackableHue(u32 hue)
{
	cellGem.todo("cellGemIsTrackableHue(hue=%d)", hue);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (hue > 359)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	std::lock_guard lock(tracker.mutex);

	return tracker.hue_is_trackable(hue);
}

error_code cellGemPrepareCamera(s32 max_exposure, f32 image_quality)
{
	cellGem.todo("cellGemPrepareCamera(max_exposure=%d, image_quality=%f)", max_exposure, image_quality);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (false) // TODO: Check if the camera is currently being prepared.
	{
		return CELL_EBUSY;
	}

	max_exposure = std::clamp(max_exposure, static_cast<s32>(CELL_GEM_MIN_CAMERA_EXPOSURE), static_cast<s32>(CELL_GEM_MAX_CAMERA_EXPOSURE));
	image_quality = std::clamp(image_quality, 0.0f, 1.0f);

	// TODO: prepare camera

	return CELL_OK;
}

error_code cellGemPrepareVideoConvert(vm::cptr<CellGemVideoConvertAttribute> vc_attribute)
{
	cellGem.warning("cellGemPrepareVideoConvert(vc_attribute=*0x%x)", vc_attribute);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!vc_attribute)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	const CellGemVideoConvertAttribute vc = *vc_attribute;

	if (vc.version != CELL_GEM_VERSION)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (vc.output_format != CELL_GEM_NO_VIDEO_OUTPUT)
	{
		if (!vc.video_data_out)
		{
			return CELL_GEM_ERROR_INVALID_PARAMETER;
		}
	}

	if ((vc.conversion_flags & CELL_GEM_COMBINE_PREVIOUS_INPUT_FRAME) && !vc.buffer_memory)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!vc.video_data_out.aligned(128) || !vc.buffer_memory.aligned(16))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	gem.vc_attribute = vc;

	const s32 buffer_size = cellGemGetVideoConvertSize(vc.output_format);
	gem.video_data_out_size = buffer_size;

	return CELL_OK;
}

error_code cellGemReadExternalPortDeviceInfo(u32 gem_num, vm::ptr<u32> ext_id, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_DEVICE_INFO_SIZE]> ext_info)
{
	cellGem.warning("cellGemReadExternalPortDeviceInfo(gem_num=%d, ext_id=*0x%x, ext_info=%s)", gem_num, ext_id, ext_info);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num) || !ext_id)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	const u64 start = get_system_time();

	auto& controller = gem.controllers[gem_num];

	if (g_cfg.io.move != move_handler::null)
	{
		// Get external device status
		CellGemExtPortData ext_port_data{};
		ds3_input_to_ext(gem_num, controller, ext_port_data);
	}

	if (!(controller.ext_status & CELL_GEM_EXT_CONNECTED))
	{
		return CELL_GEM_NO_EXTERNAL_PORT_DEVICE;
	}

	*ext_id = controller.ext_id;

	if (ext_info && g_cfg.io.move == move_handler::real)
	{
		bool read_requested = false;

		while (true)
		{
			{
				std::lock_guard lock(pad::g_pad_mutex);

				const auto handler = pad::get_current_handler();
				const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

				if (pad->m_pad_handler != pad_handler::move || !(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
				{
					return CELL_GEM_NOT_CONNECTED;
				}

				if (!read_requested)
				{
					pad->move_data.external_device_read_requested = true;
					read_requested = true;
				}

				if (!pad->move_data.external_device_read_requested)
				{
					*ext_id = controller.ext_id = pad->move_data.external_device_id;
					std::memcpy(pad->move_data.external_device_read.data(), ext_info.get_ptr(), CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE);
					break;
				}
			}

			// We wait for 300ms at most
			if (const u64 elapsed_us = get_system_time() - start; elapsed_us > 300'000)
			{
				cellGem.warning("cellGemReadExternalPortDeviceInfo(gem_num=%d): timeout", gem_num);
				break;
			}

			// TODO: sleep ?
		}
	}

	return CELL_OK;
}

error_code cellGemReset(u32 gem_num)
{
	cellGem.todo("cellGemReset(gem_num=%d)", gem_num);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.reset_controller(gem_num);

	// TODO: is this correct?
	gem.start_timestamp_us = get_guest_system_time();

	return CELL_OK;
}

error_code cellGemSetRumble(u32 gem_num, u8 rumble)
{
	cellGem.trace("cellGemSetRumble(gem_num=%d, rumble=0x%x)", gem_num, rumble);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	gem.controllers[gem_num].rumble = rumble;

	// Set actual device rumble
	if (g_cfg.io.move == move_handler::real)
	{
		std::lock_guard pad_lock(pad::g_pad_mutex);
		const auto handler = pad::get_current_handler();
		auto& handlers = handler->get_handlers();
		if (auto it = handlers.find(pad_handler::move); it != handlers.end() && it->second)
		{
			const u32 pad_index = pad_num(gem_num);
			for (const auto& binding : it->second->bindings())
			{
				if (!binding.device || binding.device->player_id != pad_index) continue;

				handler->SetRumble(pad_index, rumble, rumble > 0);
				break;
			}
		}
	}

	return CELL_OK;
}

error_code cellGemSetYaw(u32 gem_num, vm::ptr<f32> z_direction)
{
	cellGem.todo("cellGemSetYaw(gem_num=%d, z_direction=*0x%x)", gem_num, z_direction);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!z_direction || !check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	// TODO

	return CELL_OK;
}

error_code cellGemTrackHues(vm::cptr<u32> req_hues, vm::ptr<u32> res_hues)
{
	cellGem.todo("cellGemTrackHues(req_hues=%s, res_hues=*0x%x)", req_hues ? fmt::format("*0x%x [%d, %d, %d, %d]", req_hues, req_hues[0], req_hues[1], req_hues[2], req_hues[3]) : "*0x0", res_hues);

	auto& gem = g_fxo->get<gem_config>();

	std::scoped_lock lock(gem.mtx);

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!req_hues)
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	for (u32 i = 0; i < CELL_GEM_MAX_NUM; i++)
	{
		if (req_hues[i] == CELL_GEM_DONT_CARE_HUE)
		{
			gem.controllers[i].enabled_tracking = true;
			gem.controllers[i].enabled_LED = true;
			gem.controllers[i].hue_set = true;

			switch (i)
			{
			default:
			case 0:
				gem.controllers[i].hue = 240; // blue
				break;
			case 1:
				gem.controllers[i].hue = 0;   // red
				break;
			case 2:
				gem.controllers[i].hue = 120; // green
				break;
			case 3:
				gem.controllers[i].hue = 300; // purple
				break;
			}

			const auto [r, g, b] = ps_move_tracker<false>::hsv_to_rgb(gem.controllers[i].hue, 1.0f, 1.0f);
			gem.controllers[i].sphere_rgb = gem_config::gem_color(r / 255.0f, g / 255.0f, b / 255.0f);

			if (res_hues)
			{
				res_hues[i] = gem.controllers[i].hue;
			}
		}
		else if (req_hues[i] == CELL_GEM_DONT_TRACK_HUE)
		{
			gem.controllers[i].enabled_tracking = false;
			gem.controllers[i].enabled_LED = false;
			gem.controllers[i].hue_set = false;

			if (res_hues)
			{
				res_hues[i] = CELL_GEM_DONT_TRACK_HUE;
			}
		}
		else
		{
			if (req_hues[i] > 359)
			{
				cellGem.warning("cellGemTrackHues: req_hues[%d]=%d -> this can lead to unexpected behavior", i, req_hues[i]);
			}

			gem.controllers[i].enabled_tracking = true;
			gem.controllers[i].enabled_LED = true;
			gem.controllers[i].hue_set = true;
			gem.controllers[i].hue = req_hues[i];

			const auto [r, g, b] = ps_move_tracker<false>::hsv_to_rgb(gem.controllers[i].hue, 1.0f, 1.0f);
			gem.controllers[i].sphere_rgb = gem_config::gem_color(r / 255.0f, g / 255.0f, b / 255.0f);

			if (res_hues)
			{
				res_hues[i] = gem.controllers[i].hue;
			}
		}
	}

	return CELL_OK;
}

error_code cellGemUpdateFinish()
{
	cellGem.warning("cellGemUpdateFinish()");

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	std::scoped_lock lock(gem.mtx);

	if (!gem.updating)
	{
		return CELL_GEM_ERROR_UPDATE_NOT_STARTED;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();
	tracker.wait_for_result();

	gem.updating = false;

	if (!gem.camera_frame)
	{
		return not_an_error(CELL_GEM_NO_VIDEO);
	}

	return CELL_OK;
}

error_code cellGemUpdateStart(vm::cptr<void> camera_frame, u64 timestamp)
{
	cellGem.warning("cellGemUpdateStart(camera_frame=*0x%x, timestamp=%d)", camera_frame, timestamp);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	auto& tracker = g_fxo->get<named_thread<gem_tracker>>();

	if (tracker.is_busy())
	{
		return CELL_GEM_ERROR_UPDATE_NOT_FINISHED;
	}

	std::scoped_lock lock(gem.mtx);

	// Update is starting even when camera_frame is null
	if (gem.updating.exchange(true))
	{
		return CELL_GEM_ERROR_UPDATE_NOT_FINISHED;
	}

	if (!camera_frame.aligned(128))
	{
		return CELL_GEM_ERROR_INVALID_ALIGNMENT;
	}

	gem.camera_frame = camera_frame.addr();

	if (!tracker.set_image(gem.camera_frame))
	{
		return not_an_error(CELL_GEM_NO_VIDEO);
	}

	tracker.wake_up();

	return CELL_OK;
}

error_code cellGemWriteExternalPort(u32 gem_num, vm::ptr<u8[CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE]> data)
{
	cellGem.warning("cellGemWriteExternalPort(gem_num=%d, data=%s)", gem_num, data);

	auto& gem = g_fxo->get<gem_config>();

	if (!gem.state)
	{
		return CELL_GEM_ERROR_UNINITIALIZED;
	}

	if (!check_gem_num(gem_num))
	{
		return CELL_GEM_ERROR_INVALID_PARAMETER;
	}

	if (!gem.is_controller_ready(gem_num))
	{
		return CELL_GEM_NOT_CONNECTED;
	}

	if (data && g_cfg.io.move == move_handler::real)
	{
		std::lock_guard lock(pad::g_pad_mutex);

		const auto handler = pad::get_current_handler();
		const auto& pad = ::at32(handler->GetPads(), pad_num(gem_num));

		if (pad->m_pad_handler != pad_handler::move || !(pad->m_port_status & CELL_PAD_STATUS_CONNECTED))
		{
			return CELL_GEM_NOT_CONNECTED;
		}

		if (pad->move_data.external_device_write_requested)
		{
			return CELL_GEM_ERROR_WRITE_NOT_FINISHED;
		}

		std::memcpy(pad->move_data.external_device_write.data(), data.get_ptr(), CELL_GEM_EXTERNAL_PORT_OUTPUT_SIZE);
		pad->move_data.external_device_write_requested = true;
	}

	return CELL_OK;
}

DECLARE(ppu_module_manager::cellGem)("libgem", []()
{
	REG_FUNC(libgem, cellGemCalibrate);
	REG_FUNC(libgem, cellGemClearStatusFlags);
	REG_FUNC(libgem, cellGemConvertVideoFinish);
	REG_FUNC(libgem, cellGemConvertVideoStart);
	REG_FUNC(libgem, cellGemEnableCameraPitchAngleCorrection);
	REG_FUNC(libgem, cellGemEnableMagnetometer);
	REG_FUNC(libgem, cellGemEnableMagnetometer2);
	REG_FUNC(libgem, cellGemEnd);
	REG_FUNC(libgem, cellGemFilterState);
	REG_FUNC(libgem, cellGemForceRGB);
	REG_FUNC(libgem, cellGemGetAccelerometerPositionInDevice);
	REG_FUNC(libgem, cellGemGetAllTrackableHues);
	REG_FUNC(libgem, cellGemGetCameraState);
	REG_FUNC(libgem, cellGemGetEnvironmentLightingColor);
	REG_FUNC(libgem, cellGemGetHuePixels);
	REG_FUNC(libgem, cellGemGetImageState);
	REG_FUNC(libgem, cellGemGetInertialState);
	REG_FUNC(libgem, cellGemGetInfo);
	REG_FUNC(libgem, cellGemGetMemorySize);
	REG_FUNC(libgem, cellGemGetRGB);
	REG_FUNC(libgem, cellGemGetRumble);
	REG_FUNC(libgem, cellGemGetState);
	REG_FUNC(libgem, cellGemGetStatusFlags);
	REG_FUNC(libgem, cellGemGetTrackerHue);
	REG_FUNC(libgem, cellGemHSVtoRGB);
	REG_FUNC(libgem, cellGemInit);
	REG_FUNC(libgem, cellGemInvalidateCalibration);
	REG_FUNC(libgem, cellGemIsTrackableHue);
	REG_FUNC(libgem, cellGemPrepareCamera);
	REG_FUNC(libgem, cellGemPrepareVideoConvert);
	REG_FUNC(libgem, cellGemReadExternalPortDeviceInfo);
	REG_FUNC(libgem, cellGemReset);
	REG_FUNC(libgem, cellGemSetRumble);
	REG_FUNC(libgem, cellGemSetYaw);
	REG_FUNC(libgem, cellGemTrackHues);
	REG_FUNC(libgem, cellGemUpdateFinish);
	REG_FUNC(libgem, cellGemUpdateStart);
	REG_FUNC(libgem, cellGemWriteExternalPort);
});
