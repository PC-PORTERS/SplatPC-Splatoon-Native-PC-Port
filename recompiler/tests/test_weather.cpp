#include "os/gx2/gx2_weather.h"
#include "os/gx2/gx2_env_light.h"
#include "os/gx2/gx2_weather_math.h"
#include "os/gx2/gx2_weather_collision.h"
#include "os/gx2/gx2_sky_model.h"
#include "os/gx2/gx2_weather_shaders.h"
#include "os/gx2/vk/gx2_vk_shader.h"
#include "runtime/port_settings.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <fstream>

namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
gx2weather::Camera camera() {
    float data[44]{};
    data[0] = 1;
    data[5] = 1.7f;
    data[7] = -40.8f;
    data[10] = -1.0002f;
    data[11] = 146.03f;
    data[14] = -1;
    data[15] = 150;
    data[16] = data[21] = data[26] = 1;
    data[23] = -24;
    data[27] = -150;
    data[28] = data[33] = data[38] = 1;
    data[35] = 24;
    data[39] = 150;
    data[40] = 2;
    data[41] = 19998;
    gx2weather::Camera out;
    require(gx2weather::decode_camera(data, sizeof data, out), "valid camera rejected");
    require(!gx2weather::decode_camera(data, 175, out), "short camera accepted");
    data[28] = 2;
    require(!gx2weather::decode_camera(data, sizeof data, out), "object transform accepted as camera");
    data[28] = 1;
    data[0] = std::numeric_limits<float>::quiet_NaN();
    require(!gx2weather::decode_camera(data, sizeof data, out), "NaN camera accepted");
    return out;
}
void math() {
    camera();
    using namespace gx2weather;
    {
        namespace rs = rbrew_settings;
        rs::Values v;
        v.saturation = 85;
        v.gamma = 120;
        v.shadow_lift = 4;
        v.highlight_softness = 35;
        v.sharpness = 75;
        v.clarity = 130;
        v.contrast = 110;
        require(rs::save_look(v, 0), "picture preset not saved");
        const auto saved = v.looks[0];
        const auto text = rs::encode_look(saved);
        require(rs::decode_look(text, v.looks[2]) && v.looks[2].values == saved.values,
                "saved look did not round-trip through settings text");
        v.gamma = 80;
        v.contrast = 60;
        v.weather = rs::kRain;
        v.camera_motion = false;
        const int key = v.keys[0];
        require(rs::load_look(v, 2) && v.gamma == 120 && v.contrast == 110 && v.saturation == 85 && v.clarity == 130 &&
                    v.weather == rs::kRain && v.keys[0] == key && !v.camera_motion,
                "picture preset changed controls, weather or failed to restore tools");
        for (const auto bad : {std::string(""), std::string("2,1"), text.substr(0, text.size() - 2), text + ",4",
                               std::string("1,9999999999999999999999")}) {
            auto copy = saved;
            require(!rs::decode_look(bad, copy) && copy.values == saved.values,
                    "invalid saved look changed existing data");
        }
        require(!rs::load_look(v, 1) && !rs::save_look(v, 3), "empty/out-of-range preset accepted");
        v.gamma = 0;
        v.sharpness = 999;
        v.shadow_lift = -500;
        rs::normalize(v);
        require(v.gamma == 50 && v.sharpness == 200 && v.shadow_lift == -25, "picture controls left their safe bounds");
    }
    require(scene_color_extent(848, 480, false) && scene_color_extent(960, 540, false) &&
                scene_color_extent(1280, 720, true),
            "native Octo Valley or scaled scene target rejected");
    require(!scene_color_extent(320, 180, false) && !scene_color_extent(848, 480, true),
            "small auxiliary target confused with plaza main image");
    require(matching_scene_aspect(1280, 720, 848, 480) && matching_scene_aspect(73, 41, 73, 41),
            "scaled/odd-sized scene depth rejected");
    require(!matching_scene_aspect(2560, 1440, 1600, 1600) && !matching_scene_aspect(0, 720, 848, 480),
            "shadow atlas accepted as scene depth");
    for (int type : {1, 2, 3, 5, 6, 16}) {
        const auto policy = gx2env::scene_graphics(type, true, true, true);
        require(policy.environment == (type == 2 || type == 3 || type == 5 || type == 6) &&
                    policy.post_only == (type == 1),
                "mission/title sky policy violated");
    }
    require(!gx2env::scene_graphics(1, true, false).post_only && !gx2env::scene_graphics(3, false).environment,
            "scene effect switches ignored");
    {
        const std::array<float, 3> orange{1, .15f, .01f}, moon{.55f, .66f, 1};
        const auto night = gx2env::backlight_color(orange, moon, true, 1, 0);
        require(night[2] > night[0] && night[0] < .02f, "night backlight retained orange daytime fill");
        require(gx2env::backlight_color(orange, moon, true, 0, 0) == std::array<float, 3>{},
                "moon below horizon still lights skin");
        require(gx2env::backlight_color(orange, {1, 1, 1}, false, 0, 0) == orange,
                "daytime native backlight changed at white illumination");
    }
    {
        const auto c = gx2env::ambient_from_environment({1, .3f, 0}, {1, 1, 1}, {.05f, .08f, .2f});
        require(c[2] > c[0] && c[0] > 0, "orange character ambient with zero blue could not acquire the night's hue");
    }
    require(gx2env::night_lamp_intensity(400, false, 0, 20) == 0 &&
                gx2env::night_lamp_intensity(400, false, 1, 20) == 32 &&
                gx2env::night_lamp_intensity(10, true, 0, 20) == 10,
            "new night lamp leaked into day or ignored the native intensity limit");
    require(cumulus_coverage(-1, true, true) > cumulus_coverage(-1, false, true),
            "after-rain cloud banks are not fuller than clear weather");
    require(cumulus_coverage(0, true, true) == 0 && cumulus_coverage(100, true, true) == 1 &&
                cumulus_coverage(100, true, false) == 0,
            "dense cloud coverage ignored amount or off switch");
    {
        rbrew_settings::Values v;
        v.cumulus_amount = 999;
        rbrew_settings::normalize(v);
        require(v.cumulus_amount == 100, "cumulus slider overflow");
        v.cumulus_amount = -10;
        rbrew_settings::normalize(v);
        require(v.cumulus_amount == -1, "cumulus auto value rejected");
    }
    {
        CloudClock clock;
        require(clock.advance(10, 100) == 0 && clock.advance(10.5, 100) == .5, "cloud clock did not follow real time");
        require(clock.advance(11, 0) == .5 && clock.advance(11.5, 400) == 2.5,
                "cloud speed did not stop or scale all evolution");
        require(clock.advance(100, 100) == 2.5 && clock.advance(3, 100) == 2.5 && clock.advance(3.5, 100) == 3,
                "time changes moved the cloud bank abruptly");
        require(clock.advance(std::numeric_limits<double>::quiet_NaN(), 100) == 3 && clock.advance(4, 100) == 3.5,
                "invalid time poisoned the cloud clock");
    }
    {
        gx2env::MainCameraIdentity ids;
        require(!ids.matches(10, 100, false), "unknown camera accepted without matching the player eye");
        require(ids.matches(10, 100, true) && ids.matches(10, 200, true), "plaza camera objects not identified");
        require(ids.matches(10, 100, false) && ids.matches(10, 200, false),
                "upward/collision view lost the identified camera");
        require(!ids.matches(10, 300, false), "shadow camera accepted as main view");
        require(!ids.matches(11, 100, false) && ids.matches(11, 300, true), "camera identity leaked into a new scene");
        require(!ids.matches(0, 300, true) && !ids.matches(11, 300, false), "camera identity survived scene reset");
    }
    {
        const float pos[3] = {10, 24, 150}, front[3] = {10, 24, 149}, right[3] = {11, 24, 150}, up[3] = {0, 1, 0},
                    high[3] = {10, 25, 149};
        Camera view;
        require(look_at_camera(pos, front, up, 55, 2, 20000, 16.f / 9.f, view), "main view camera rejected");
        require(std::abs(view.inverse_view[0] - 1) < 1e-6f && std::abs(view.inverse_view[5] - 1) < 1e-6f &&
                    std::abs(view.inverse_view[10] - 1) < 1e-6f,
                "front-facing main view has a rotated basis");
        const auto original = view;
        require(look_at_camera(pos, right, up, 55, 2, 20000, 16.f / 9.f, view) &&
                    std::abs(view.inverse_view[2] + 1) < 1e-6f && std::abs(view.inverse_view[10]) < 1e-6f,
                "sky centre ray did not rotate with camera yaw");
        require(look_at_camera(pos, high, up, 55, 2, 20000, 16.f / 9.f, view) &&
                    std::abs(view.inverse_view[6] + std::sqrt(.5f)) < 1e-6f &&
                    std::abs(view.inverse_view[10] - std::sqrt(.5f)) < 1e-6f,
                "sky centre ray did not follow upward camera pitch");
        require(view.inverse_view[3] == 10 && view.inverse_view[7] == 24 && view.inverse_view[11] == 150,
                "camera construction lost world position");
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned other = 0; other < 3; ++other) {
                float dot = 0;
                for (unsigned k = 0; k < 3; ++k)
                    dot += view.inverse_view[row * 4 + k] * view.inverse_view[other * 4 + k];
                require(std::abs(dot - (row == other ? 1.f : 0.f)) < 1e-6f, "look-at basis is not orthonormal");
            }
        const auto before = view;
        require(!look_at_camera(pos, pos, up, 55, 2, 20000, 16.f / 9.f, view) &&
                    view.inverse_view == before.inverse_view,
                "degenerate look-at overwrote the valid camera");
        require(!look_at_camera(pos, front, up, 0, 2, 20000, 16.f / 9.f, view) &&
                    !look_at_camera(pos, front, up, 55, 20000, 2, 16.f / 9.f, view),
                "invalid camera projection accepted");
        float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
        require(!look_at_camera(bad, front, up, 55, 2, 20000, 16.f / 9.f, view), "non-finite main camera accepted");
        require(look_at_camera(pos, front, up, 75, 2, 20000, 16.f / 9.f, view) &&
                    view.projection[1] < original.projection[1],
                "main camera ignored changed FOV");
        std::puts("camera: world position, yaw, upward pitch, basis, FOV and invalid input passed");
    }
    require(mesh_hash("hello", 5) == 0xA430D84680AABD0Bull, "mesh hash changed");
    require(is_sky_mesh(0x821FD18E81E01923ull, 7024, 16, 2280), "verified sky mesh rejected");
    require(
        !is_sky_mesh(0x821FD18E81E01923ull, 7024, 16, 2279) && !is_sky_mesh(0x821FD18E81E01923ull, 7023, 16, 2280) &&
            !is_sky_mesh(0x821FD18E81E01923ull, 7024, 12, 2280) && !is_sky_mesh(0x821FD18E81E01922ull, 7024, 16, 2280),
        "unrelated geometry classified as sky");
    require(day_hour(360, true, 24, 0) == 6 && day_hour(360, true, 24, 1440) == 6, "day rollover changed start time");
    require(day_hour(1080, false, 24, 99999) == 18, "fixed clock advanced");
    require(sun_direction(12)[1] > .9f && sun_direction(0)[1] < -.9f, "sun elevation or night is wrong");
    {
        const auto dusk_sun = gx2sky::celestial(18.6).sun;
        const float len = std::sqrt(dusk_sun[0] * dusk_sun[0] + dusk_sun[2] * dusk_sun[2]);
        require(dusk_sun[2] / len < -0.98f, "the sun does not set in the plaza's main view");
        require(gx2sky::celestial(0).sun_elevation < -20 && gx2sky::celestial(13).sun_elevation > 55,
                "sun path is wrong");
        require(gx2sky::celestial(5.0).sun_elevation < 0 && gx2sky::celestial(5.5).sun_elevation > 0,
                "sunrise is not near 05:15");
        const auto dusk = gx2sky::sun_transmittance(3);
        require(dusk[0] > dusk[1] && dusk[1] > dusk[2] && dusk[2] < 0.3f, "low sun is not reddened");
    }
    require(std::abs(sun_direction(6)[1]) < .001f && std::abs(sun_direction(18)[1]) < .001f,
            "sunrise/sunset timing is wrong");
    for (int quality = 0; quality < rbrew_settings::kShaderQualityCount; ++quality)
        for (int fps : {30, 60, 120, 180}) {
            rbrew_settings::Values v;
            v.shader_quality = quality;
            v.frame_rate = fps;
            v.plaza_60fps = false;
            rbrew_settings::normalize(v);
            // The online edition hides the graphics presets: normalize() always turns them off.
            require(v.shader_quality == rbrew_settings::kShadersOff, "shader presets were not forced off");
            require(v.frame_rate == fps, "the frame rate was changed while the shaders are off");
            require(v.fps120 == (fps >= 120), "interpolation does not follow the frame rate");
            require(!v.plaza_60fps, "the plaza's 30 FPS setting was changed while the shaders are off");
        }
    rbrew_settings::Values bad;
    bad.shader_quality = 99;
    bad.weather = -4;
    bad.time_of_day = 9000;
    bad.day_length = 0;
    bad.bloom = 1000;
    rbrew_settings::normalize(bad);
    require(bad.shader_quality == rbrew_settings::kShadersOff && bad.weather == 0 && bad.time_of_day == 1439 && bad.day_length == 5 &&
                bad.bloom == 100,
            "weather settings escaped supported bounds");
    std::puts("weather: camera validation, clock rollover, sun path and mandatory shader FPS cap passed");
}
void collision() {
    using namespace gx2weather;
    std::vector<uint8_t> fixture(200);
    auto word = [&](size_t at, uint32_t v) {
        for (unsigned i = 0; i < 4; ++i) fixture[at + i] = uint8_t(v >> (24 - i * 8));
    };
    auto number = [&](size_t at, float v) { word(at, std::bit_cast<uint32_t>(v)); };
    word(0, 0x02020000);
    word(8, 56);
    word(12, 1);
    word(56, 60);
    word(60, 60);
    word(64, 72);
    word(68, 120);
    word(72, 140);
    const Point normals[] = {{0, -1, 0}, {-1, 0, 0}, {0, 0, -1}, {.70710678f, 0, .70710678f}};
    for (unsigned i = 0; i < 4; ++i) {
        number(132 + i * 12, normals[i].x);
        number(136 + i * 12, normals[i].y);
        number(140 + i * 12, normals[i].z);
    }
    number(180, 7.0710678f);
    fixture[189] = 1;
    fixture[191] = 2;
    fixture[193] = 3;
    std::vector<RoofTriangle> triangles;
    require(collision_triangles(fixture, triangles) && triangles.size() == 1, "valid collision prism rejected");
    require(std::abs(triangles[0].b.x - 10) < .001 && std::abs(triangles[0].c.z - 10) < .001,
            "collision triangle reconstructed incorrectly");
    fixture[186] = 255;
    fixture[187] = 255;
    require(!collision_triangles(fixture, triangles), "invalid collision normal accepted");
    fixture.resize(56);
    require(!collision_triangles(fixture, triangles), "truncated collision model accepted");
    const uint8_t yaz[] = {'Y', 'a', 'z', '0', 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0xe0, 'a', 'b', 'c', 0x10, 2};
    std::vector<uint8_t> unpacked;
    require(unpack_yaz(yaz, unpacked) && std::string(unpacked.begin(), unpacked.end()) == "abcabc",
            "Yaz overlap copy failed");
    require(!unpack_yaz(std::span(yaz).first(20), unpacked), "truncated Yaz copy accepted");
    auto bad_yaz = std::vector<uint8_t>(std::begin(yaz), std::end(yaz));
    bad_yaz[16] = 0;
    require(!unpack_yaz(bad_yaz, unpacked), "Yaz reference before output accepted");
    fixture.assign(96, 0);
    std::memcpy(fixture.data(), "SARC", 4);
    fixture[5] = 20;
    fixture[6] = 254;
    fixture[7] = 255;
    word(12, 80);
    std::memcpy(fixture.data() + 20, "SFAT", 4);
    fixture[27] = 1;
    word(36, 0x01000000);
    word(44, 4);
    std::memcpy(fixture.data() + 48, "SFNT", 4);
    std::memcpy(fixture.data() + 56, "roof.kcl", 9);
    fixture[83] = 4;
    require(archive_member(fixture, "roof.kcl").size() == 4 && archive_member(fixture, "roof.kcl")[3] == 4,
            "archive member extraction failed");
    word(44, 99);
    require(archive_member(fixture, "roof.kcl").empty(), "archive range escaped file");
    const RoofTriangle surfaces[] = {{{-20, 0, -20}, {20, 0, -20}, {20, 0, 20}},
                                     {{-20, 0, -20}, {20, 0, 20}, {-20, 0, 20}},
                                     {{-5, 10, -5}, {5, 10, -5}, {5, 10, 5}},
                                     {{-5, 10, -5}, {5, 10, 5}, {-5, 10, 5}}};
    const auto cover = rain_cover(surfaces, 128);
    auto height = [&](Point p) {
        const int x = int((p.x + p.y * 18 / 140 - cover.mapping[0]) * cover.mapping[2] * cover.size);
        const int z = int((p.z + p.y * 5 / 140 - cover.mapping[1]) * cover.mapping[3] * cover.size);
        return cover.heights[size_t(z) * cover.size + x];
    };
    require(height({0, 1, 0}) > 9.99f, "off-screen roof does not shelter rain");
    require(std::abs(height({15, 1, 0})) < .001f, "open street incorrectly sheltered");
    require(rain_cover(surfaces, 0).heights.empty(), "invalid roof resolution accepted");
    std::puts(
        "collision: prism reconstruction, archive/Yaz bounds, wind-projected roof shelter and open street passed");
}
int gpu() {
    if (!gx2vk::init(nullptr)) return 77;
    auto make = [](VkFormat format, VkImageUsageFlags extra, unsigned width = 73, unsigned height = 41) {
        gx2vk::Image image;
        gx2vk::ImageDesc d;
        d.width = width;
        d.height = height;
        d.format = format;
        d.usage =
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra;
        require(gx2vk::create_image(image, d), "weather test image creation failed");
        return image;
    };
    auto color = make(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT),
         depth = make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    {
        auto current = make(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        auto history = make(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        auto next = make(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        auto output = make(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
        const auto device = gx2vk::device();
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {12, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 6;
        li.pBindings = bindings;
        VkDescriptorSetLayout set_layout;
        require(vkCreateDescriptorSetLayout(device, &li, nullptr, &set_layout) == VK_SUCCESS,
                "TAA test descriptor layout failed");
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout;
        VkPipelineLayout layout;
        require(vkCreatePipelineLayout(device, &pli, nullptr, &layout) == VK_SUCCESS,
                "TAA test pipeline layout failed");
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 3;
        pi.pPoolSizes = sizes;
        VkDescriptorPool pool;
        require(vkCreateDescriptorPool(device, &pi, nullptr, &pool) == VK_SUCCESS, "TAA test descriptor pool failed");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set;
        require(vkAllocateDescriptorSets(device, &ai, &set) == VK_SUCCESS, "TAA test descriptor allocation failed");
        const auto module =
            gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, std::string(gx2weather::kCommon) + gx2weather::kTemporal);
        require(module != VK_NULL_HANDLE, "TAA shader compilation failed");
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = layout;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        VkPipeline pipeline;
        require(vkCreateComputePipelines(device, gx2vk::pipeline_cache(), 1, &ci, nullptr, &pipeline) == VK_SUCCESS,
                "TAA test pipeline failed");
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const auto sampler = gx2vk::sampler(si);
        auto upload_image = [&](gx2vk::Image& image, const std::vector<uint16_t>& pixels) {
            auto span = gx2vk::upload(pixels.size() * 2, 16);
            require(span.data != nullptr, "TAA test image upload failed");
            std::memcpy(span.data, pixels.data(), pixels.size() * 2);
            VkBufferImageCopy copy{};
            copy.bufferOffset = span.offset;
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {73, 41, 1};
            gx2vk::barrier_if_dirty();
            vkCmdCopyBufferToImage(gx2vk::cmd(), span.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
            gx2vk::note_gpu_write(image);
        };
        auto image_view = [](gx2vk::Image& image, bool is_depth = false) {
            gx2vk::ViewDesc d;
            d.aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            return gx2vk::image_view(image, d);
        };
        for (unsigned test = 0; test < 5; ++test) {
            const unsigned x = test == 3 ? 54 : 36, y = 20;
            std::vector<uint16_t> pixels(73 * 41 * 4), old(pixels.size());
            for (unsigned row = 0; row < 41; ++row)
                for (unsigned col = 0; col < 73; ++col) {
                    const size_t at = (row * 73 + col) * 4;
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        pixels[at + channel] = ((row + col) & 1) ? 0x3c00 : 0;
                        old[at + channel] = test == 3 && col < 49 ? 0x3400 : 0x3a00;
                    }
                    pixels[at + 3] = 0x3900;
                    old[at + 3] = test == 1 ? 0x4600 : (test == 2 || test == 4) ? 0xbc00 : 0x4200;
                }
            for (unsigned channel = 0; channel < 3; ++channel) pixels[(y * 73 + x) * 4 + channel] = 0x3400;
            upload_image(current, pixels);
            upload_image(history, old);
            gx2vk::barrier_if_dirty();
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            const VkClearDepthStencilValue value{test == 4 ? 1.f : (1000.f - 1000.f / 8) / (1000.f - 1), 0};
            vkCmdClearDepthStencilImage(gx2vk::cmd(), depth.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            gx2vk::note_gpu_write(depth);
            gx2weather::WeatherParams p{};
            p.cam0[0] = p.cam1[1] = p.cam2[2] = p.prev0[0] = p.prev1[1] = p.prev2[2] = 1;
            p.proj[0] = p.proj[1] = p.prev_proj[1] = 1;
            p.prev_proj[0] = test == 3 ? .5f : 1.f;
            p.proj[2] = p.prev_proj[2] = 1;
            p.proj[3] = p.prev_proj[3] = 1000;
            p.depth_range[1] = 1;
            p.taa[0] = 1;
            p.taa[1] = .88f;
            p.cloud_control[1] = test == 4 ? 1.f : 0.f;
            const auto uniform = gx2vk::upload(
                sizeof p, std::max<VkDeviceSize>(16, gx2vk::caps().limits.minUniformBufferOffsetAlignment));
            require(uniform.data != nullptr, "TAA test uniform upload failed");
            std::memcpy(uniform.data, &p, sizeof p);
            const VkDescriptorBufferInfo buffer{uniform.buffer, uniform.offset, sizeof p};
            const VkDescriptorImageInfo images[] = {{sampler, image_view(depth, true), VK_IMAGE_LAYOUT_GENERAL},
                                                    {sampler, image_view(current), VK_IMAGE_LAYOUT_GENERAL},
                                                    {VK_NULL_HANDLE, image_view(output), VK_IMAGE_LAYOUT_GENERAL},
                                                    {VK_NULL_HANDLE, image_view(next), VK_IMAGE_LAYOUT_GENERAL},
                                                    {sampler, image_view(history), VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet writes[6]{};
            for (unsigned i = 0; i < 6; ++i) {
                writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                writes[i].dstSet = set;
                writes[i].dstBinding = bindings[i].binding;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = bindings[i].descriptorType;
                if (i == 0)
                    writes[i].pBufferInfo = &buffer;
                else
                    writes[i].pImageInfo = &images[i - 1];
            }
            vkUpdateDescriptorSets(device, 6, writes, 0, nullptr);
            gx2vk::barrier_if_dirty();
            vkCmdBindPipeline(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(gx2vk::cmd(), 10, 6, 1);
            gx2vk::note_gpu_write(output);
            gx2vk::note_gpu_write(next);
            std::vector<uint8_t> raw;
            require(gx2vk::read_image(output, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, raw),
                    "TAA test readback failed");
            uint16_t rgb[4];
            std::memcpy(rgb, raw.data() + (y * 73 + x) * 8, 8);
            require(test == 0 ? rgb[0] > 0x3800 : rgb[0] == 0x3400,
                    "TAA retained hidden history or used the current FOV for the old frame");
            require(rgb[3] == 0x3900, "TAA changed display alpha");
            require(gx2vk::read_image(next, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, raw),
                    "TAA history readback failed");
            std::memcpy(rgb, raw.data() + (y * 73 + x) * 8, 8);
            require(rgb[3] == (test == 4 ? 0xbc00 : 0x4200), "TAA did not save the current view depth");
        }
        const auto air_module =
            gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, std::string(gx2weather::kCommon) + R"glsl(
layout(local_size_x=8,local_size_y=1) in;
layout(binding=10,rgba16f) uniform writeonly image2D outImage;
void main() {
    uint i=gl_GlobalInvocationID.x;if(i>=8)return;
    const float distances[6]=float[6](15.0,100.0,300.0,500.0,2000.0,2000.0);
    float value=i<6?airOpacity(distances[i],i==5?-1.0:.000018):
        sunDiscVisibility(i==6?vec2(.5):vec2(-.1,.5),vec2(.02));
    const float heights[6]=float[6](-.1,0.0,.15,.5,1.0,1.1);
    float height=heights[min(i,5u)];
    imageStore(outImage,ivec2(i,0),vec4(value,cloudHeightProfile(height,0.0),cloudHeightProfile(height,1.0),1));
}
)glsl");
        require(air_module != VK_NULL_HANDLE, "air/occlusion probe shader failed");
        ci.stage.module = air_module;
        VkPipeline air_pipeline;
        require(vkCreateComputePipelines(device, gx2vk::pipeline_cache(), 1, &ci, nullptr, &air_pipeline) == VK_SUCCESS,
                "air/occlusion probe pipeline failed");
        for (unsigned blocked = 0; blocked < 2; ++blocked) {
            gx2vk::barrier_if_dirty();
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            const VkClearDepthStencilValue value{blocked ? .8f : 1.f, 0};
            vkCmdClearDepthStencilImage(gx2vk::cmd(), depth.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            gx2vk::note_gpu_write(depth);
            gx2vk::barrier_if_dirty();
            vkCmdBindPipeline(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, air_pipeline);
            vkCmdBindDescriptorSets(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdDispatch(gx2vk::cmd(), 1, 1, 1);
            gx2vk::note_gpu_write(output);
            std::vector<uint8_t> raw;
            require(gx2vk::read_image(output, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, raw),
                    "air/occlusion readback failed");
            uint16_t values[8];
            for (unsigned i = 0; i < 8; ++i) std::memcpy(&values[i], raw.data() + i * 8, 2);
            require(values[0] == 0 && values[1] == 0 && values[2] == 0, "automatic haze reached the foreground");
            require(values[3] > 0 && values[4] > values[3] && values[4] < 0x3800 && values[5] == 0,
                    "air haze did not increase safely with distance");
            require(values[6] == (blocked ? 0 : 0x3c00) && values[7] == 0,
                    "occluded/off-screen sun still drives lens glare");
            uint16_t cloud[6][4];
            std::memcpy(cloud, raw.data(), sizeof cloud);
            for (unsigned i = 0; i < 6; ++i)
                for (unsigned channel = 1; channel < 3; ++channel)
                    require(cloud[i][channel] <= 0x3c00, "cloud profile left the finite 0..1 range");
            for (unsigned i : {0u, 1u, 4u, 5u})
                require(cloud[i][1] == 0 && cloud[i][2] == 0, "cloud profile leaked outside its layer");
            require(cloud[2][1] == 0x3c00 && cloud[3][1] > 0 && cloud[3][1] < cloud[3][2],
                    "cloud profile lost a full base, thinning top or deeper rain layer");
        }
        vkDestroyPipeline(device, air_pipeline, nullptr);
        const auto wet_module =
            gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, std::string(gx2weather::kCommon) + R"glsl(
layout(local_size_x=8,local_size_y=1) in;
layout(binding=10,rgba16f) uniform writeonly image2D outImage;
void main(){
    uint i=gl_GlobalInvocationID.x;if(i>=8)return;
    vec2 grain=i==0?vec2(0):vec2(.4,-.25),ripple=i==4?vec2(2,-2):i==7?vec2(100000,-100000):vec2(0);
    float puddle=i==3 || i==4 || i==5 || i==7?1.0:0.0;
    vec3 n=wetSurfaceNormal(vec3(0,1,0),grain,ripple,puddle,i==2?20.0:0.0);
    imageStore(outImage,ivec2(i,0),vec4(n,wetReflectionAmount(1.0,1.0,puddle)));
})glsl");
        require(wet_module != VK_NULL_HANDLE, "wet normal probe failed");
        ci.stage.module = wet_module;
        VkPipeline wet_pipeline;
        require(vkCreateComputePipelines(device, gx2vk::pipeline_cache(), 1, &ci, nullptr, &wet_pipeline) == VK_SUCCESS,
                "wet normal probe pipeline failed");
        vkCmdBindPipeline(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, wet_pipeline);
        vkCmdBindDescriptorSets(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(gx2vk::cmd(), 1, 1, 1);
        gx2vk::note_gpu_write(output);
        std::vector<uint8_t> wet_raw;
        require(gx2vk::read_image(output, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, wet_raw),
                "wet normal readback failed");
        uint16_t normals[8][4];
        std::memcpy(normals, wet_raw.data(), sizeof normals);
        for (unsigned i = 0; i < 8; ++i)
            require((normals[i][0] & 0x7fff) < 0x2800 && normals[i][1] > 0x3b00 && normals[i][1] <= 0x3c00 &&
                        (normals[i][2] & 0x7fff) < 0x2800,
                    "wet surface normal left its bounded finite range");
        for (unsigned i : {0u, 3u, 5u})
            require(normals[i][0] == 0 && normals[i][1] == 0x3c00 && normals[i][2] == 0,
                    "still water acquired a random reflection normal");
        require(normals[1][0] == normals[6][0] && normals[1][2] == normals[6][2],
                "same world microstructure depended on screen pixel");
        require(normals[2][0] < normals[1][0] && normals[4][0] > 0 && normals[7][0] > 0,
                "microstructure filtering or bounded rain ripples missing");
        require(normals[1][3] < 0x3000 && normals[3][3] > 0x3800 && normals[3][3] <= 0x38cd,
                "wet asphalt became as glossy as standing water");
        vkDestroyPipeline(device, wet_pipeline, nullptr);
        const auto cloud_module =
            gx2vk::shader_module(VK_SHADER_STAGE_COMPUTE_BIT, std::string(gx2weather::kCommon) + R"glsl(
layout(local_size_x=8,local_size_y=1) in;
layout(binding=10,rgba16f) uniform writeonly image2D outImage;
void main(){
    uint i=gl_GlobalInvocationID.x;
    vec4 c;
    if(i==0)c=vec4(cloudScattering(1,0,38),cloudScattering(1,.2,38),cloudScattering(0,0,38),starVisibility(.1,0));
    else if(i==1)c=vec4(exposedCloud(vec3(100,50,25),1),starVisibility(-.3,0));
    else if(i==2)c=vec4(exposedCloud(vec3(100,50,25),8),starVisibility(-.3,1));
    else if(i==3)c=vec4(exposedCloud(vec3(0),40),starVisibility(-.1,0));
    else if(i==4)c=vec4(pictureTools(vec3(.05,.2,.8),vec4(1,0,0,1)),1);
    else if(i==5)c=vec4(pictureTools(vec3(0),vec4(1,.1,0,1)),1);
    else if(i==6)c=vec4(pictureTools(vec3(2),vec4(1,0,1,1)),1);
    else c=vec4(pictureTools(vec3(.25),vec4(1.5,0,0,1)),1);
    imageStore(outImage,ivec2(i,0),c);
})glsl");
        require(cloud_module != VK_NULL_HANDLE, "cloud optics probe compilation failed");
        ci.stage.module = cloud_module;
        VkPipeline cloud_pipeline;
        require(vkCreateComputePipelines(device, gx2vk::pipeline_cache(), 1, &ci, nullptr, &cloud_pipeline) ==
                    VK_SUCCESS,
                "cloud optics probe pipeline failed");
        vkCmdBindPipeline(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, cloud_pipeline);
        vkCmdBindDescriptorSets(gx2vk::cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdDispatch(gx2vk::cmd(), 1, 1, 1);
        gx2vk::note_gpu_write(output);
        std::vector<uint8_t> optics_raw;
        require(gx2vk::read_image(output, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, optics_raw),
                "cloud optics readback failed");
        uint16_t optics[8][4];
        std::memcpy(optics, optics_raw.data(), sizeof optics);
        for (auto& pixel : optics)
            for (auto v : pixel) require(v < 0x7c00, "cloud optics produced non-finite or negative output");
        require(optics[0][0] > optics[0][1] && optics[0][0] > optics[0][2],
                "dense cloud interior brighter than its sunlit edge");
        require(optics[0][3] == 0 && optics[1][3] == 0x3c00 && optics[2][3] == 0 && optics[3][3] > 0 &&
                    optics[3][3] < 0x3c00,
                "daylight stars or broken twilight transition");
        auto half = [](uint16_t v) { return v ? std::ldexp(float(1024 + (v & 1023)), int(v >> 10) - 25) : 0.f; };
        require(std::abs(half(optics[1][0]) / half(optics[1][1]) - 2) < .005f &&
                    std::abs(half(optics[1][1]) / half(optics[1][2]) - 2) < .005f,
                "cloud shoulder changed colour ratios");
        require(optics[2][0] > optics[1][0] && half(optics[2][0]) < 16 && optics[3][0] == 0 && optics[3][1] == 0 &&
                    optics[3][2] == 0,
                "cloud exposure lost highlight detail or lifted black");
        require(std::abs(half(optics[4][0]) - .05) < .001 && std::abs(half(optics[4][1]) - .2) < .001 &&
                    std::abs(half(optics[4][2]) - .8) < .001,
                "neutral picture controls changed colour");
        require(std::abs(half(optics[5][0]) - .1) < .001 && std::abs(half(optics[6][0]) - .875) < .001 &&
                    std::abs(half(optics[7][0]) - .39685) < .002,
                "shadow, highlight or gamma tools did not match their controls");
        vkDestroyPipeline(device, cloud_pipeline, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyPipelineLayout(device, layout, nullptr);
        vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
        for (auto* image : {&current, &history, &next, &output}) gx2vk::destroy_image(*image);
        gx2vk::finish();
        std::puts("TAA GPU: valid accumulation, disocclusion, sky rejection, changed FOV and display alpha passed");
        std::puts("air GPU: clear foreground, gradual distance haze, blocked/off-screen sun rejection passed");
        std::puts("cloud GPU: bounded density profile, clear layer boundaries and deeper rain clouds passed");
        std::puts("wet GPU: stable world grain, distance filtering, flat puddles and bounded ripples passed");
        std::puts(
            "cloud optics GPU: self shadow, forward rim, hue-preserving shoulder and day/twilight/night stars passed");
    }
    const gx2weather::RoofTriangle ground[] = {{{-500, 0, -500}, {500, 0, -500}, {500, 0, 500}},
                                               {{-500, 0, -500}, {500, 0, 500}, {-500, 0, 500}}};
    require(gx2weather::set_rain_cover(gx2weather::rain_cover(ground, 64)), "collision cover GPU upload failed");
    auto reset = [&](float depth_value = 1.f) {
        gx2vk::barrier_if_dirty();
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue value{{.2f, .3f, .4f, .625f}};
        vkCmdClearColorImage(gx2vk::cmd(), color.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
        range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        const VkClearDepthStencilValue z{depth_value, 0};
        vkCmdClearDepthStencilImage(gx2vk::cmd(), depth.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
        gx2vk::note_gpu_write(color);
        gx2vk::note_gpu_write(depth);
    };
    auto read = [&] {
        std::vector<uint8_t> raw;
        require(gx2vk::read_image(color, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 73, 41, raw), "weather readback failed");
        require(raw.size() == 73 * 41 * 16, "weather output extent changed");
        double mean = 0;
        for (size_t i = 0; i < raw.size(); i += 16) {
            float c[4];
            std::memcpy(c, raw.data() + i, 16);
            for (float value : c) require(std::isfinite(value) && value >= 0, "non-finite or negative weather output");
            require(c[3] == .625f, "weather changed scene alpha");
            mean += c[0] + c[1] + c[2];
        }
        return mean / (73 * 41 * 3);
    };
    const auto cam = camera();
    rbrew_settings::Values v;
    v.film_look = false;
    v.antialiasing = false; // grain and TAA history change every frame
    v.day_cycle = false;
    v.clouds = false;
    v.bloom = 0;
    v.sun_rays = false;
    v.time_of_day = 720;
    reset();
    v.shader_quality = 0;
    require(!gx2weather::apply(color, depth, cam, v, 0), "Off mode applied shaders");
    require(std::abs(read() - .3) < 1e-6, "Off mode modified pixels");
    reset();
    v.shader_quality = 2;
    {
        auto atlas = make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 41, 41);
        require(!gx2weather::apply(color, atlas, cam, v, 0), "shadow-atlas depth reached the scene post-process");
        require(std::abs(read() - .3) < 1e-6, "rejected shadow depth changed the scene");
        gx2vk::destroy_image(atlas);
    }
    require(gx2weather::apply(color, depth, cam, v, 0), "weather shader compilation or dispatch failed");
    const double noon = read();
    auto reverse = cam;
    reverse.depth_range = {1, 0};
    reset(0);
    require(gx2weather::apply(color, depth, reverse, v, 0), "reversed depth dispatch failed");
    require(std::abs(read() - noon) < 1e-5, "reversed sky depth changed atmosphere");
    reset();
    v.time_of_day = 0;
    require(gx2weather::apply(color, depth, cam, v, 0), "night dispatch failed");
    // Night darkens through the game's own lights (gx2_env_light), not this pass.
    // Run beyond a descriptor pool generation, changing image extents by quality.
    for (unsigned i = 0; i < 70; ++i) {
        reset();
        v.shader_quality = 1 + int(i % 3);
        v.weather = int(i % 3);
        v.clouds = true;
        v.sun_rays = true;
        v.time_of_day = (i % 4) * 360;
        require(gx2weather::apply(color, depth, cam, v, i * .17), "weather pool/quality transition failed");
    }
    read();
    require(gx2vk::finish(), "weather finish failed");
    v.shader_quality = 2;
    v.antialiasing = true;
    v.motion_blur = false;
    v.sun_rays = false;
    v.clouds = false;
    v.bloom = 0;
    v.film_look = false;
    for (unsigned width : {73u, 91u, 73u}) {
        auto c = make(VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, width, 41);
        auto d = make(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, width, 41);
        for (int frame = 0; frame < 3; ++frame) {
            gx2vk::barrier_if_dirty();
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            const VkClearColorValue value{{.2f, .3f, .4f, .625f}};
            vkCmdClearColorImage(gx2vk::cmd(), c.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            gx2vk::note_gpu_write(c);
            range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            const VkClearDepthStencilValue z{.8f, 0};
            vkCmdClearDepthStencilImage(gx2vk::cmd(), d.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &range);
            gx2vk::note_gpu_write(d);
            auto moved = cam;
            moved.projection[0] *= frame == 1 ? 1.15f : 1.f;
            require(gx2weather::apply(c, d, moved, v, 20 + frame),
                    "TAA history resize/field-of-view integration failed");
        }
        std::vector<uint8_t> raw;
        require(gx2vk::read_image(c, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, width, 41, raw), "resized TAA readback failed");
        for (size_t i = 0; i < raw.size(); i += 16) {
            float pixel[4];
            std::memcpy(pixel, raw.data() + i, 16);
            require(std::isfinite(pixel[0]) && std::isfinite(pixel[1]) && std::isfinite(pixel[2]) && pixel[3] == .625f,
                    "resized TAA corrupted colour/alpha");
        }
        gx2vk::destroy_image(c);
        gx2vk::destroy_image(d);
    }
    gx2vk::finish();
    gx2weather::shutdown();
    gx2vk::destroy_image(color);
    gx2vk::destroy_image(depth);
    gx2vk::finish();
    require(gx2vk::validation_error_count() == 0, gx2vk::last_validation_error().c_str());
    gx2vk::shader_shutdown();
    gx2vk::shutdown();
    std::puts("weather GPU: finite odd-size output, alpha, original Off, day/night and 70 in-flight quality/weather "
              "changes passed");
    return 0;
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc > 1 && std::string_view(argv[1]) == "looks-write") {
            rbrew_settings::Values v;
            v.gamma = 115;
            v.saturation = 90;
            v.clarity = 125;
            require(rbrew_settings::save_look(v, 1), "look file fixture failed");
            rbrew_settings::set(v);
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "looks-read") {
            rbrew_settings::load();
            auto v = rbrew_settings::get();
            require(v.gamma == 115 && v.saturation == 90 && v.clarity == 125 && v.looks[1].saved,
                    "picture controls/preset did not survive process restart");
            v.gamma = 50;
            require(rbrew_settings::load_look(v, 1) && v.gamma == 115,
                    "persisted preset did not restore its picture controls");
            std::puts("picture settings: controls and custom look survive process restart");
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "gpu") return gpu();
        if (argc > 2 && std::string_view(argv[1]) == "collision") {
            std::ifstream file(argv[2], std::ios::binary);
            std::vector<uint8_t> raw((std::istreambuf_iterator<char>(file)), {});
            std::vector<gx2weather::RoofTriangle> triangles;
            require(gx2weather::collision_triangles(raw, triangles), "local plaza KCL failed validation");
            const auto cover = gx2weather::rain_cover(triangles);
            require(!cover.heights.empty(), "plaza roof map failed");
            std::printf("real plaza collision: %zu triangles, %u x %u wind-projected roof samples\n", triangles.size(),
                        cover.size, cover.size);
            return 0;
        }
        math();
        collision();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
}
