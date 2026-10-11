#include "ikv/renderer.hpp"
#include "ikv/vulkan_rt.hpp"
#include "ikv/fsr_vk.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <thread>
#include <unordered_map>
#include <utility>

#ifndef IKV_SHADER_DIR
#define IKV_SHADER_DIR "shaders/vulkan"
#endif
namespace ikv { namespace {
void checked(VkResult r,const char* operation){if(r!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": Vulkan result "+std::to_string(r));}
// A scene-load memory block shared by many buffers/images; freed with the last resource holding it (retained-reload
// releases keep their range until then). vkFreeMemory also unmaps.
struct MemoryBlock {
    VkDevice device=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize size=0;std::byte* mapped=nullptr;
    MemoryBlock()=default;MemoryBlock(const MemoryBlock&)=delete;MemoryBlock& operator=(const MemoryBlock&)=delete;
    ~MemoryBlock(){if(memory)vkFreeMemory(device,memory,nullptr);}
};
// One full scene load binds its ~20k buffers and images into a few blocks per memory type instead of one vkAllocateMemory
// each (65 % of the 2.2-2.4 s Eminönü load, wave-12 Perf §4). Buffers and images never share a block, so
// bufferImageGranularity never applies; blocks grow 16 MiB..256 MiB.
struct MemoryPool {
    std::array<std::shared_ptr<MemoryBlock>,2*VK_MAX_MEMORY_TYPES> blocks; // [type*2 + image]
    std::array<VkDeviceSize,2*VK_MAX_MEMORY_TYPES> used{},next{};
    std::size_t allocations=0;
};
// memory: a dedicated allocation; block: pooled (memory stays null, the block owns it).
struct Buffer { VkBuffer handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize bytes=0;void* mapped=nullptr;std::shared_ptr<MemoryBlock> block; };
struct Image {VkImage handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkImageView view=VK_NULL_HANDLE;std::uint32_t width=0,height=0,levels=1;VkFormat format=VK_FORMAT_UNDEFINED;std::shared_ptr<MemoryBlock> block;};
// jitter: xy this frame's / zw the previous frame's sub-pixel projection offset in NDC (vp/previous_vp include it).
struct Frame {Matrix4 vp,light;std::array<float,4> eye,sun,params,viewport,ambient,diffuse,fog_color,fog,sky_b,sky_c;Matrix4 previous_vp;std::array<float,4> camera_right,camera_up,camera_forward,previous_eye,previous_forward,source_time,jitter;};
static_assert(sizeof(Frame)==464 && offsetof(Frame,previous_vp)==288 && offsetof(Frame,source_time)==432 && offsetof(Frame,jitter)==448);
// previous_model: last frame's model matrix (scene.vert motion vectors only; other shaders declare the 128-byte prefix).
struct Draw {Matrix4 model;std::array<float,4> color,flags,extra,scroll;Matrix4 previous_model;};
static_assert(sizeof(Draw)==192 && offsetof(Draw,scroll)==112 && offsetof(Draw,previous_model)==128);
// Skinned meshes write staging[frame slot] on the host; the copy into vertices is recorded into that frame (upload).
// previous: the pose the last frame drew (motion vectors; swapped with vertices on upload); previous_stale: previous
// still differs from vertices.
struct Geometry {Buffer vertices,indices,staging[2],previous;std::uint32_t count=0;std::uint64_t revision=0;std::array<float,4> bounds{};bool upload=false,previous_stale=false;};
// Two frames in flight: frame serial s records/submits slot s&1 and the CPU waits only for the frame two serials back.
struct FrameSlot {VkCommandBuffer command=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkSemaphore acquire=VK_NULL_HANDLE;bool pending=false;std::uint64_t serial=0;};
struct DrawItem {std::uint32_t mesh,material;Matrix4 model;float distance=0;bool alpha=false,sky=false,fog=false,dynamic=false;std::array<float,4> scroll{};bool visible=true;Matrix4 previous{};};
class VulkanRenderer final:public Renderer {
 SDL_Window* window_;RendererOptions options_;VkInstance instance_=VK_NULL_HANDLE;VkSurfaceKHR surface_=VK_NULL_HANDLE;VkPhysicalDevice physical_=VK_NULL_HANDLE;VkDevice device_=VK_NULL_HANDLE;VkQueue queue_=VK_NULL_HANDLE;std::uint32_t family_=0;
 VkSwapchainKHR swapchain_=VK_NULL_HANDLE;VkFormat swap_format_=VK_FORMAT_UNDEFINED;VkExtent2D extent_{},render_extent_{};std::vector<VkImage> swap_images_;std::vector<VkSemaphore> present_semaphores_;VkCommandPool pool_=VK_NULL_HANDLE;
 FrameSlot slots_[2];VkCommandBuffer command_=VK_NULL_HANDLE; // command_: the current slot's buffer while render() records
 // post_sets_/temporal_sets_ [i] read history hdr_[i]; written with the scene descriptors, never while a frame is pending.
 VkSampler sampler_=VK_NULL_HANDLE,shadow_sampler_=VK_NULL_HANDLE,linear_clamp_=VK_NULL_HANDLE;VkDescriptorSetLayout set_layout_=VK_NULL_HANDLE,rt_layout_=VK_NULL_HANDLE;VkPipelineLayout pipeline_layout_=VK_NULL_HANDLE;VkDescriptorPool descriptor_pool_=VK_NULL_HANDLE;VkDescriptorSet post_sets_[2]{},rt_set_=VK_NULL_HANDLE,temporal_sets_[2]{};
 VkRenderPass scene_pass_=VK_NULL_HANDLE,shadow_pass_=VK_NULL_HANDLE,post_pass_=VK_NULL_HANDLE,temporal_pass_=VK_NULL_HANDLE,after_pass_=VK_NULL_HANDLE;VkFramebuffer scene_fb_[2]{},temporal_fb_[2]{},after_fb_[2]{},shadow_fb_=VK_NULL_HANDLE,post_fb_=VK_NULL_HANDLE;VkPipeline opaque_=VK_NULL_HANDLE,alpha_=VK_NULL_HANDLE,shadow_pipeline_=VK_NULL_HANDLE,post_pipeline_=VK_NULL_HANDLE,overlay_pipeline_=VK_NULL_HANDLE,temporal_pipeline_=VK_NULL_HANDLE,contour_pipeline_=VK_NULL_HANDLE;VkPipelineCache cache_=VK_NULL_HANDLE;
 VkPipeline opaque_double_=VK_NULL_HANDLE,alpha_double_=VK_NULL_HANDLE,shadow_double_=VK_NULL_HANDLE;
 VkPipeline sky_pipeline_[4]{},particle_pipeline_[5]{},after_particle_pipeline_[5]{};
 // particle_drawn_[i]: serial of the last frame that drew particle_buffer_[i].
 VkDescriptorPool particle_pool_=VK_NULL_HANDLE;std::vector<Image> particle_images_;std::vector<VkDescriptorSet> particle_sets_;Buffer particle_buffer_[2];unsigned particle_slot_=0;std::uint64_t particle_drawn_[2]{};std::vector<ParticleBatch> particle_batches_;
 // Original fake HDR (0046fdb0, original appearance only): 128x128 X8R8G8B8 targets bb38/bb3c (00466e40) = glow_chain_[0]/[1].
 // glow_sets_: [0]/[1] half_intensity from hdr_[0]/hdr_[1] with bb38, [2] reads bb3c, [3] reads bb38.
 VkRenderPass glow_pass_=VK_NULL_HANDLE;VkFramebuffer glow_fb_[2]{};VkPipeline glow_pipeline_=VK_NULL_HANDLE,glow_composite_pipeline_=VK_NULL_HANDLE;VkDescriptorSet glow_sets_[4]{};
 Image hdr_[2],raw_hdr_,guide_[2],depth_,shadow_,output_,overlay_,white_,flat_normal_,glow_chain_[2];std::vector<Image> textures_;std::unordered_map<std::string,std::size_t> texture_lookup_;std::vector<Geometry> geometry_;std::vector<VkDescriptorSet> material_sets_;
 // Colour locations per render pass (VK_FORMAT_UNDEFINED = VK_ATTACHMENT_UNUSED). Scene pass: 0 HDR colour, 1 RT guide,
 // 2 motion (RG16F previousUV-UV, FSR or RT denoise), 3/4 RT illumination/reflection history, 5 FSR reactive (R8).
 std::vector<VkFormat> scene_colors_,after_colors_,temporal_colors_{VK_FORMAT_R16G16B16A16_SFLOAT};
 const std::vector<VkFormat>& colors_of(VkRenderPass pass)const {
     static const std::vector<VkFormat> single{VK_FORMAT_R16G16B16A16_SFLOAT};
     return pass==scene_pass_?scene_colors_:pass==after_pass_?after_colors_:pass==temporal_pass_?temporal_colors_:single;
 }
 static std::uint32_t attachments(const std::vector<VkFormat>& colors){return static_cast<std::uint32_t>(std::count_if(colors.begin(),colors.end(),[](VkFormat f){return f!=VK_FORMAT_UNDEFINED;}));}
 // FSR 3.1.4: motion_/reactive_ render-resolution inputs, fsr_output_ the upscaled colour post.frag samples.
 Image motion_,reactive_,fsr_output_;std::unique_ptr<Fsr3Upscaler> fsr_;bool motion_vectors_=false;
// Guided RT: scene locations 3/4 hold this frame's stochastic indirect gain / reflection; the temporal pass writes
// their accumulated estimates (diffuse alpha = history length) to *_history_[slot] and reads the other slot. Three
// a-trous passes then filter that accumulation every frame: history[slot] -> A -> B -> A, atrous_ = {A diffuse,
// A reflection, B diffuse, B reflection}; each pass also composes the display colour into hdr_[slot].
Image rt_diffuse_,rt_reflection_,diffuse_history_[2],reflection_history_[2],atrous_[4];
VkFramebuffer atrous_fb_[2][2]{};VkDescriptorSet atrous_sets_[2][3]{}; // [slot][A/B target], [slot][iteration]
 std::uint64_t jitter_frame_=0;std::chrono::steady_clock::time_point last_render_{};bool fsr_reset_=true; // reset: new targets/scene
 // Frame uniforms: one aligned region per slot behind the dynamic binding 0 of every set (uniform_offset_ while recording).
 Buffer uniform_;std::uint32_t uniform_stride_=0,uniform_offset_=0;
 struct ViewsLess {bool operator()(const std::array<VkImageView,5>& a,const std::array<VkImageView,5>& b)const{return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),std::less<VkImageView>{});}};
 std::map<std::array<VkImageView,5>,VkDescriptorSet,ViewsLess> material_cache_;std::vector<VkDescriptorPool> material_pools_; // see material_set()
 struct ContourItem {std::uint32_t mesh,material;Matrix4 model;std::array<float,3> color;};
 std::vector<ContourItem> contours_;
 std::vector<std::uint8_t> material_usage_;
 std::vector<Matrix4> history_instance_models_;
 std::unique_ptr<VulkanRtScene> rt_;
 const Scene* scene_=nullptr;
 std::size_t instance_count_=0;
 std::vector<DrawItem> draws_;
 SourceScrollState source_scroll_;
 bool overlay_visible_=false,rt_enabled_=false,denoise_enabled_=false,fsr_supported_=false,dirty_=true,rendered_=false,grayscale_=false;
 bool bc_supported_=false; // BC1/2/3 sampled natively (textureCompressionBC + linear filtering)
 // Original lighting equation (lighting.glsl originalSurface, display-referred, fog, lightmaps, glow): original appearance,
 // or any appearance with faithful RT (RtStyle). Enhanced appearance keeps its sampling/FSR either way.
 bool original_lighting_=true;
 Frame previous_frame_{};
 std::uint64_t history_eligible_frames_=0,history_reset_events_=0,history_retained_dirty_frames_=0;
 std::uint32_t frame_=0;
 std::uint64_t transform_hash_=0,color_resource_hash_=0,geometry_revision_=0;
 std::vector<std::uint64_t> mesh_keys_; // Scene mesh keys as last loaded; actor attach/detach remaps geometry_ by them.
 std::uint64_t mesh_key_=0; // Scene::mesh_key as last loaded: any actor (re)assembly since then reloads.
 std::uint64_t total_frames_=0;
 std::filesystem::path cache_path_;
 // Host-written per-frame resources are indexed by the frame slot (total_frames_&1), so HUD/particle/skin uploads wait only
 // for that slot's previous frame. Overlay staging copies are recorded into the next frame's command buffer.
 Buffer overlay_staging_[2];bool overlay_upload_=false,overlay_undefined_=false,overlay_whole_=false;
 std::vector<VkBufferImageCopy> overlay_regions_; // pending changed rectangles of the next upload (unused while overlay_whole_)
 bool source_presented_=false;
 // FSR 3.1.4 frame generation (generating_ = fsr_->generates()): per frame slot, generated_ is the frame interpolated
 // between the previous and this frame's HUD-less output_, real_ this frame; hud_pass_ composites the current HUD onto
 // both (the SDK's HUD-less rule: the scene is interpolated, the HUD never is). The present thread then owns the
 // swapchain: it acquires, copies and presents generated_, then real_ half a rendered-frame interval later.
 Image generated_[2],real_[2];VkRenderPass hud_pass_=VK_NULL_HANDLE;VkFramebuffer hud_fb_[3]{}; // generated_[0/1], output_
 VkPipeline hud_pipeline_=VK_NULL_HANDLE; // overlay.frag for hud_pass_ (its LOAD dependencies differ from post_pass_)
 bool generating_=false,generated_last_=false;std::uint64_t generation_frame_=0;
 std::mutex queue_mutex_; // vkQueueSubmit/vkQueuePresentKHR/vkDeviceWaitIdle: the present thread shares queue_
 struct PresentJob {unsigned slot;bool generated;VkFence fence;std::chrono::steady_clock::time_point submitted;};
 std::thread presenter_;std::mutex present_mutex_;std::condition_variable present_cv_;std::deque<PresentJob> present_jobs_;
 std::uint64_t present_queued_=0,present_done_=0;bool present_stop_=false;std::exception_ptr present_error_;
 std::atomic<bool> present_stale_{false}; // the present thread saw an out-of-date/suboptimal swapchain
 VkCommandPool present_pool_=VK_NULL_HANDLE;VkCommandBuffer present_commands_[2]{};VkFence present_fences_[2]{};
 VkSemaphore present_acquire_[2]{}; // [0] generated, [1] real
 // VK_EXT_present_timing (device + surface support; used on frame-generation swapchains, timed_swapchain_): first-pixel-out
 // display times in timing_domain_ (CLOCK_MONOTONIC when offered) for the cadence log, read without blocking.
 bool present_timing_=false,timed_swapchain_=false,timing_monotonic_=false;std::uint64_t timing_domain_=0;
 PFN_vkGetPastPresentationTimingEXT past_timing_=nullptr;PFN_vkSetSwapchainPresentTimingQueueSizeEXT timing_queue_=nullptr;
 PFN_vkGetSwapchainTimeDomainPropertiesEXT time_domains_=nullptr;PFN_vkGetCalibratedTimestampsKHR calibrated_=nullptr;
 double timing_offset_ms_=0; // timing_domain_ + offset = CLOCK_MONOTONIC (timing_monotonic_: offset known)
 std::uint64_t present_id_=0;float pacing_lead_ms_=2;
 std::chrono::steady_clock::time_point log_time_{};std::uint64_t log_frame_=0; // frame log: average rendered period
 // Buffers the frames up to serial `first` may still use (released retained-reload geometry, in-frame upload staging):
 // destroyed once that frame completed. rt_stale_: a retained reload left the RT scene for render() to update in-frame.
 std::vector<std::pair<std::uint64_t,Buffer>> retired_;bool rt_stale_=false;
 void retire(Buffer& b){if(b.handle)retired_.emplace_back(total_frames_,std::exchange(b,Buffer{}));}
 void progress()const {if(options_.progress)options_.progress();}
 void wait_fence(VkFence fence,const char* operation)const {
     for(;;) {
         progress();
         const auto result=vkWaitForFences(device_,1,&fence,VK_TRUE,50'000'000);
         if(result==VK_TIMEOUT)continue;
         checked(result,operation);break;
     }
     progress();
 }
 void drain_fence(VkFence fence)const noexcept {
     // Unwinding must retain command/staging/destination ownership until completion, without callbacks.
     VkResult result;
     do {result=vkWaitForFences(device_,1,&fence,VK_TRUE,UINT64_MAX);}
     while(result!=VK_SUCCESS&&result!=VK_ERROR_DEVICE_LOST);
 }
 void wait_slot(unsigned slot,const char* operation) {
     auto& s=slots_[slot];
     if(!s.pending)return;
     wait_fence(s.fence,operation);s.pending=false;
     // A fence signal also covers every earlier submission, so all frames up to this serial completed.
     std::erase_if(retired_,[&](auto& entry){if(entry.first>s.serial)return false;destroy(entry.second);return true;});
 }
 void wait_serial(std::uint64_t serial,const char* operation) {
     if(slots_[serial&1u].serial==serial)wait_slot(serial&1u,operation);
 }
 void wait_frame(const char* operation) { // every frame in flight
     for(unsigned slot=0;slot<2;++slot)wait_slot(slot,operation);
 }
 void bind_set(VkDescriptorSet set) {
     vkCmdBindDescriptorSets(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout_,0,1,&set,1,&uniform_offset_);
 }
 void destroy(Geometry& g){destroy(g.vertices);destroy(g.indices);for(auto& b:g.staging)destroy(b);destroy(g.previous);g.upload=g.previous_stale=false;}
 void wait_idle(const char* operation) {
     wait_frame(operation);drain_presents();
     // A frame fence does not retire WSI presentation/semaphore use. Keep real device idle
     // after draining renderer work; uploads and RT builds drain their owned fences locally.
     progress();
     {std::scoped_lock lock(queue_mutex_);checked(vkDeviceWaitIdle(device_),operation);}
     progress();
 }
 VkResult queue_submit(const VkSubmitInfo& submit,VkFence fence){std::scoped_lock lock(queue_mutex_);return vkQueueSubmit(queue_,1,&submit,fence);}
 // Frame generation: wait until at most `outstanding` present jobs remain (pumping progress(), which is main-thread
 // only), then rethrow a present-thread failure.
 void drain_presents(std::uint64_t outstanding=0) {
     const auto drained=[&]{return present_queued_-present_done_<=outstanding;};
     std::unique_lock lock(present_mutex_);
     while(!present_cv_.wait_for(lock,std::chrono::milliseconds(50),drained)) {
         lock.unlock();progress();lock.lock();
     }
     if(present_error_)std::rethrow_exception(std::exchange(present_error_,nullptr));
 }
 // Present thread: acquire a swapchain image, copy source (finished: the job's frame fence was waited) into it and
 // present. Returns the present id, 0 when nothing was presented.
 std::uint64_t present_image(unsigned index,const Image& source) {
     auto& fence=present_fences_[index];const auto command=present_commands_[index];
     while(vkWaitForFences(device_,1,&fence,VK_TRUE,50'000'000)==VK_TIMEOUT){}
     std::uint32_t image=0;VkResult acquired;
     do acquired=vkAcquireNextImageKHR(device_,swapchain_,50'000'000,present_acquire_[index],VK_NULL_HANDLE,&image);
     while(acquired==VK_TIMEOUT||acquired==VK_NOT_READY);
     if(acquired==VK_ERROR_OUT_OF_DATE_KHR){present_stale_=true;return 0;}
     if(acquired==VK_SUBOPTIMAL_KHR)present_stale_=true;
     else checked(acquired,"acquire image");
     const VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
     VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
     submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&present_acquire_[index];submit.pWaitDstStageMask=&stage;
     checked(vkResetCommandBuffer(command,0),"reset present command");
     VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
     checked(vkBeginCommandBuffer(command,&begin),"begin present command");
     barrier(command,swap_images_[image],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
     // source was written by the frame submission, earlier on this queue.
     VkMemoryBarrier written{};written.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
     written.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;written.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
     vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&written,0,nullptr,0,nullptr);
     VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
     copy.extent={extent_.width,extent_.height,1};
     vkCmdCopyImage(command,source.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swap_images_[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
     barrier(command,swap_images_[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
         VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
     checked(vkEndCommandBuffer(command),"end present command");
     checked(vkResetFences(device_,1,&fence),"reset present fence");
     submit.commandBufferCount=1;submit.pCommandBuffers=&command;
     submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&present_semaphores_[image];
     const std::uint64_t id=++present_id_;
     VkPresentId2KHR present_id{};present_id.sType=VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR;present_id.swapchainCount=1;present_id.pPresentIds=&id;
     VkPresentTimingInfoEXT timing{};timing.sType=VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT;timing.timeDomainId=timing_domain_;
     timing.presentStageQueries=VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
     VkPresentTimingsInfoEXT timings{};timings.sType=VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT;timings.pNext=&present_id;
     timings.swapchainCount=1;timings.pTimingInfos=&timing;
     VkPresentInfoKHR present{};present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;present.pNext=timed_swapchain_?&timings:nullptr;
     present.waitSemaphoreCount=1;present.pWaitSemaphores=&present_semaphores_[image];
     present.swapchainCount=1;present.pSwapchains=&swapchain_;present.pImageIndices=&image;
     VkResult result;
     {
         std::scoped_lock lock(queue_mutex_);
         checked(vkQueueSubmit(queue_,1,&submit,fence),"submit present copy");
         result=vkQueuePresentKHR(queue_,&present);
     }
     if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)present_stale_=true;
     else checked(result,"present frame");
     return result==VK_ERROR_OUT_OF_DATE_KHR?0:id;
 }
 // Present thread (owns the swapchain while generating_). Pacing: the generated frame is presented as soon as its frame
 // completed on the GPU, the real frame half the measured rendered-frame interval later (minus pacing_lead_ms_, so it
 // reaches the compositor before that refresh). Presents never wait for display (FIFO shows each image for at least one
 // refresh, in order). Display times come from VK_EXT_present_timing when the swapchain reports them (non-blocking),
 // otherwise present-call times are logged. Never calls progress(), which is main-thread only.
 void present_loop()noexcept {
     using clock=std::chrono::steady_clock;
     const auto ms=[](clock::duration d){return std::chrono::duration<double,std::milli>(d).count();};
     struct Shown {std::uint64_t id;clock::time_point submitted;bool real;};
     std::deque<Shown> unconfirmed; // presented on timed_swapchain_, display time not reported yet
     clock::time_point last_ready{};double last_display=-1; // ms in the reporting time domain
     double interval_ms=0,latency_ms=0;std::vector<double> intervals;std::uint64_t generated=0,real=0,latency_count=0;
     const auto shown=[&](const Shown& s,double display_ms,bool monotonic) {
         if(last_display>=0)intervals.push_back(display_ms-last_display);
         last_display=display_ms;
         if(s.real)++real;else ++generated;
         if(s.real&&monotonic){++latency_count;latency_ms+=display_ms-ms(s.submitted.time_since_epoch());}
         if(intervals.size()<600)return;
         auto sorted=intervals;std::sort(sorted.begin(),sorted.end());
         const double mean=std::accumulate(sorted.begin(),sorted.end(),0.)/double(sorted.size());
         double variance=0;for(double v:sorted)variance+=(v-mean)*(v-mean);
         const auto at=[&](double q){return sorted[std::min(sorted.size()-1,std::size_t(q*double(sorted.size())))];};
         std::cerr<<"Vulkan frame generation presents="<<sorted.size()<<" generated="<<generated<<" real="<<real
             <<" displayIntervalMs mean="<<mean<<" p1="<<at(.01)<<" p50="<<at(.5)<<" p99="<<at(.99)<<" max="<<sorted.back()
             <<" sd="<<std::sqrt(variance/double(sorted.size()))<<" fps="<<1000/mean
             <<" timing="<<(timed_swapchain_?"firstPixelOut":"presentCall")<<" renderedFrameMs="<<interval_ms
             <<" submitToRealDisplayMs="<<(latency_count?latency_ms/double(latency_count):0)<<'\n';
         intervals.clear();generated=real=latency_count=0;latency_ms=0;
     };
     // Display times reported so far (complete reports only, in present order).
     std::array<VkPresentStageTimeEXT,64> stages{};std::array<VkPastPresentationTimingEXT,64> reports{};
     const auto collect=[&] {
         if(unconfirmed.empty())return;
         for(std::size_t i=0;i<reports.size();++i) {
             reports[i]={};reports[i].sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT;
             reports[i].presentStageCount=1;reports[i].pPresentStages=&stages[i];
         }
         VkPastPresentationTimingInfoEXT info{};info.sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT;info.swapchain=swapchain_;
         VkPastPresentationTimingPropertiesEXT past{};past.sType=VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT;
         past.presentationTimingCount=static_cast<std::uint32_t>(reports.size());past.pPresentationTimings=reports.data();
         const auto result=past_timing_(device_,&info,&past);
         if(result!=VK_SUCCESS&&result!=VK_INCOMPLETE)return;
         for(std::uint32_t i=0;i<past.presentationTimingCount;++i) {
             const auto& report=reports[i];
             while(!unconfirmed.empty()&&unconfirmed.front().id<report.presentId)unconfirmed.pop_front(); // not reported
             if(unconfirmed.empty()||unconfirmed.front().id!=report.presentId||!report.reportComplete||!report.presentStageCount||!stages[i].time)
                 continue;
             shown(unconfirmed.front(),double(stages[i].time)*1e-6+timing_offset_ms_,timing_monotonic_);unconfirmed.pop_front();
         }
     };
     for(;;) {
         PresentJob job{};
         {
             std::unique_lock lock(present_mutex_);
             present_cv_.wait(lock,[&]{return present_stop_||!present_jobs_.empty();});
             if(present_jobs_.empty())return;
             job=present_jobs_.front();
         }
         try {
             // The frame's GPU work: its fence (the render thread resets it only after this job is done).
             while(vkWaitForFences(device_,1,&job.fence,VK_TRUE,50'000'000)==VK_TIMEOUT){}
             checked(vkGetFenceStatus(device_,job.fence),"present frame fence");
             const auto ready=clock::now();
             const double period=last_ready==clock::time_point{}?0:ms(ready-last_ready);
             last_ready=ready;
             // One swapchain/compositor stall must not inflate the pacing interval (and with it the half-interval sleep that
             // throttles the render thread through drain_presents) for the next ~40 frames: samples are clamped to 1.5x the
             // running interval, so a genuine slowdown still converges (+5% per frame). analysis/porting-wave12/Perf/report.md
             if(period>0&&period<250)interval_ms=interval_ms>0?interval_ms*.9+std::min(period,interval_ms*1.5)*.1:period;
             collect();
             const auto present=[&](unsigned index,const Image& source,bool is_real) {
                 const auto id=present_image(index,source);
                 if(!id)return;
                 if(timed_swapchain_){unconfirmed.push_back({id,job.submitted,is_real});if(unconfirmed.size()>256)unconfirmed.pop_front();}
                 else shown({id,job.submitted,is_real},ms(clock::now().time_since_epoch()),true);
             };
             if(job.generated) {
                 present(0,generated_[job.slot],false);
                 std::this_thread::sleep_until(ready+std::chrono::duration_cast<clock::duration>(
                     std::chrono::duration<double,std::milli>(interval_ms*.5-pacing_lead_ms_)));
             }
             present(1,real_[job.slot],true);
         } catch(...) {
             std::scoped_lock lock(present_mutex_);
             if(!present_error_)present_error_=std::current_exception();
         }
         {std::scoped_lock lock(present_mutex_);present_jobs_.pop_front();++present_done_;}
         present_cv_.notify_all();
     }
 }
 // Frame generation tail of a frame (after the post pass wrote this frame's HUD-less image to output_): interpolates the
 // frame halfway from the previous one into generated_[slot], composites the current HUD onto both and keeps this frame
 // in real_[slot] for the present thread. reset: no usable previous frame (FSR history reset).
 void record_generation(unsigned slot,unsigned current,float frame_ms,bool reset) {
     const auto c=command_;auto& g=generated_[slot];auto& r=real_[slot];
     barrier(c,output_.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT,
         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT);
     // The present job of frame total_frames_-2 copied generated_[slot]/real_[slot] in earlier submissions.
     barrier(c,g.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
     constexpr VkImageUsageFlags output_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
     fsr_->interpolate({c,{output_.handle,output_.format,extent_,output_usage},
         {g.handle,g.format,extent_,output_usage|VK_IMAGE_USAGE_STORAGE_BIT},frame_ms,options_.camera.near_plane,
         options_.camera.far_plane,options_.camera.fov*.017453292519943295f,reset,++generation_frame_});
     constexpr VkAccessFlags hud_access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_TRANSFER_READ_BIT;
     constexpr VkPipelineStageFlags hud_stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
     barrier(c,g.handle,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_SHADER_WRITE_BIT,hud_access,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,hud_stages);
     barrier(c,output_.handle,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,0,hud_access,
         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,hud_stages);
     if(overlay_visible_)for(unsigned target:{slot,2u}) {
         begin_pass(c,hud_pass_,hud_fb_[target],extent_,false);
         bind_set(post_sets_[current]);
         vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,hud_pipeline_);vkCmdDraw(c,3,1,0,0);
         vkCmdEndRenderPass(c);
     }
     barrier(c,r.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
     VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
     copy.extent={extent_.width,extent_.height,1};
     vkCmdCopyImage(c,output_.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,r.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
     barrier(c,r.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,
         VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
 }
 void copy_staging(void* destination,const void* source,std::size_t bytes)const {
     constexpr std::size_t chunk=1024*1024;
     for(std::size_t offset=0;offset<bytes;) {
         progress();
         const auto size=std::min(chunk,bytes-offset);
         std::memcpy(static_cast<std::byte*>(destination)+offset,static_cast<const std::byte*>(source)+offset,size);
         offset+=size;
     }
     progress();
 }
 static void validate_cache(std::vector<char>& data,const VkPhysicalDeviceProperties& properties) {
     if(data.empty())return;
     std::uint32_t header[4]{};
     if(data.size()<sizeof(header)+VK_UUID_SIZE){data.clear();return;}
     std::memcpy(header,data.data(),sizeof(header));
     if(header[0]<sizeof(header)+VK_UUID_SIZE||header[0]>data.size()||
        header[1]!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE||
        header[2]!=properties.vendorID||header[3]!=properties.deviceID||
        std::memcmp(data.data()+sizeof(header),properties.pipelineCacheUUID,VK_UUID_SIZE))
         data.clear();
 }
 bool compute_queue()const {
     std::uint32_t count=0;vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,nullptr);
     std::vector<VkQueueFamilyProperties> properties(count);
     vkGetPhysicalDeviceQueueFamilyProperties(physical_,&count,properties.data());
     return family_<count&&(properties[family_].queueFlags&VK_QUEUE_COMPUTE_BIT);
 }
 void check_image_format(std::uint32_t width,std::uint32_t height,VkFormat format,VkImageUsageFlags usage)const {
     VkImageFormatProperties properties{};
     const auto result=vkGetPhysicalDeviceImageFormatProperties(physical_,format,VK_IMAGE_TYPE_2D,
         VK_IMAGE_TILING_OPTIMAL,usage,0,&properties);
     if(result!=VK_SUCCESS||width>properties.maxExtent.width||height>properties.maxExtent.height)
         throw std::runtime_error("unsupported Vulkan image format/usage/extent: format="+std::to_string(format)+
             " usage="+std::to_string(usage)+" result="+std::to_string(result));
     // Float32 geometry guides are exclusively nearest/texelFetch sampled.
     if((usage&VK_IMAGE_USAGE_SAMPLED_BIT)&&format!=VK_FORMAT_D32_SFLOAT&&format!=VK_FORMAT_R32G32B32A32_SFLOAT) {
         VkFormatProperties features{};vkGetPhysicalDeviceFormatProperties(physical_,format,&features);
         if(!(features.optimalTilingFeatures&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
             throw std::runtime_error("Vulkan linear sampled image format unavailable: "+std::to_string(format));
     }
 }
 std::uint32_t memory_type(std::uint32_t mask,VkMemoryPropertyFlags flags){VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(physical_,&p);for(std::uint32_t i=0;i<p.memoryTypeCount;++i)if((mask&(1u<<i))&&(p.memoryTypes[i].propertyFlags&flags)==flags)return i;throw std::runtime_error("Vulkan compatible memory type unavailable");}
// Binds a dedicated allocation (pool null) or a range of the pool's current block for this memory type.
template<class Bind>void bind_memory(const VkMemoryRequirements& r,VkMemoryPropertyFlags properties,MemoryPool* pool,bool image,
                                     VkDeviceMemory& memory,std::shared_ptr<MemoryBlock>& owner,void** mapped,Bind bind) {
    const auto type=memory_type(r.memoryTypeBits,properties);
    const bool host=(properties&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)!=0;
    if(!pool) {
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.allocationSize=r.size;a.memoryTypeIndex=type;
        checked(vkAllocateMemory(device_,&a,nullptr,&memory),"allocate memory");
        checked(bind(memory,VkDeviceSize(0)),"bind memory");
        if(host)checked(vkMapMemory(device_,memory,0,VK_WHOLE_SIZE,0,mapped),"map memory");
        return;
    }
    const auto slot=type*2+(image?1:0);auto& block=pool->blocks[slot];
    const auto alignment=std::max<VkDeviceSize>(1,r.alignment);
    VkDeviceSize offset=(pool->used[slot]+alignment-1)/alignment*alignment;
    if(!block||offset>block->size||r.size>block->size-offset) {
        constexpr VkDeviceSize first=16ull<<20,largest=256ull<<20;
        auto& next=pool->next[slot];next=next?std::min(next*2,largest):first;
        auto fresh=std::make_shared<MemoryBlock>();fresh->device=device_;
        VkMemoryAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;a.memoryTypeIndex=type;
        a.allocationSize=std::max(next,r.size);
        // Nearly full VRAM: a large block can fail where the resource alone still fits.
        if(vkAllocateMemory(device_,&a,nullptr,&fresh->memory)!=VK_SUCCESS) {
            a.allocationSize=r.size;checked(vkAllocateMemory(device_,&a,nullptr,&fresh->memory),"allocate memory block");
        }
        fresh->size=a.allocationSize;++pool->allocations;
        if(host){void* base=nullptr;checked(vkMapMemory(device_,fresh->memory,0,VK_WHOLE_SIZE,0,&base),"map memory block");fresh->mapped=static_cast<std::byte*>(base);}
        block=std::move(fresh);offset=0;
    }
    checked(bind(block->memory,offset),"bind pooled memory");
    if(host)*mapped=block->mapped+offset;
    pool->used[slot]=offset+r.size;owner=block;
}
Buffer buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags properties,MemoryPool* pool=nullptr) {
    Buffer b;b.bytes=size;
    VkBufferCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;c.size=size;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    checked(vkCreateBuffer(device_,&c,nullptr,&b.handle),"create buffer");
    try {
        VkMemoryRequirements r;vkGetBufferMemoryRequirements(device_,b.handle,&r);
        bind_memory(r,properties,pool,false,b.memory,b.block,&b.mapped,
            [&](VkDeviceMemory memory,VkDeviceSize offset){return vkBindBufferMemory(device_,b.handle,memory,offset);});
    } catch(...) {destroy(b);throw;}
    return b;
}
void destroy(Buffer& b){if(b.mapped&&b.memory)vkUnmapMemory(device_,b.memory);if(b.handle)vkDestroyBuffer(device_,b.handle,nullptr);if(b.memory)vkFreeMemory(device_,b.memory,nullptr);b={};}
Image image(std::uint32_t w,std::uint32_t h,VkFormat format,VkImageUsageFlags usage,
            VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT,std::uint32_t levels=1,MemoryPool* pool=nullptr) {
    if(!w||!h||!levels||levels>static_cast<std::uint32_t>(std::bit_width(std::max(w,h))))throw std::runtime_error("invalid Vulkan image mip extent");
    progress();
    Image i;i.width=w;i.height=h;i.format=format;i.levels=levels;
    VkImageCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    c.imageType=VK_IMAGE_TYPE_2D;c.format=format;c.extent={w,h,1};c.mipLevels=levels;c.arrayLayers=1;
    c.samples=VK_SAMPLE_COUNT_1_BIT;c.tiling=VK_IMAGE_TILING_OPTIMAL;c.usage=usage;c.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    check_image_format(w,h,format,usage);
    checked(vkCreateImage(device_,&c,nullptr,&i.handle),"create image");
    try {
        VkMemoryRequirements r;vkGetImageMemoryRequirements(device_,i.handle,&r);
        bind_memory(r,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,pool,true,i.memory,i.block,nullptr,
            [&](VkDeviceMemory memory,VkDeviceSize offset){return vkBindImageMemory(device_,i.handle,memory,offset);});
        VkImageViewCreateInfo v{};v.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        v.image=i.handle;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=format;v.subresourceRange={aspect,0,levels,0,1};
        checked(vkCreateImageView(device_,&v,nullptr,&i.view),"create image view");
        progress();
    } catch(...) {destroy(i);throw;}
    return i;
}
void destroy(Image& i){if(i.view)vkDestroyImageView(device_,i.view,nullptr);if(i.handle)vkDestroyImage(device_,i.handle,nullptr);if(i.memory)vkFreeMemory(device_,i.memory,nullptr);i={};}
 void barrier(VkCommandBuffer c,VkImage image,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,
              VkPipelineStageFlags before,VkPipelineStageFlags after,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT,
              std::uint32_t levels=1) {
     VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
     b.srcAccessMask=src;b.dstAccessMask=dst;b.oldLayout=old;b.newLayout=next;
     b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
     b.image=image;b.subresourceRange={aspect,0,levels,0,1};
     vkCmdPipelineBarrier(c,before,after,0,0,nullptr,0,nullptr,1,&b);
 }
 template<class F>void immediate(F fn) {
     VkCommandBuffer c=VK_NULL_HANDLE;
     VkCommandBufferAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
     a.commandPool=pool_;a.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;a.commandBufferCount=1;
     checked(vkAllocateCommandBuffers(device_,&a,&c),"allocate upload command");
     VkFence f=VK_NULL_HANDLE;bool pending=false;
     try {
         VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
         begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
         checked(vkBeginCommandBuffer(c,&begin),"begin upload");fn(c);
         checked(vkEndCommandBuffer(c),"end upload");
         VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
         checked(vkCreateFence(device_,&fc,nullptr,&f),"upload fence");
         VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
         submit.commandBufferCount=1;submit.pCommandBuffers=&c;
         checked(queue_submit(submit,f),"submit upload");pending=true;
         wait_fence(f,"wait upload");pending=false;
     } catch(...) {
         if(pending)drain_fence(f);
         if(f)vkDestroyFence(device_,f,nullptr);
         vkFreeCommandBuffers(device_,pool_,1,&c);throw;
     }
     vkDestroyFence(device_,f,nullptr);vkFreeCommandBuffers(device_,pool_,1,&c);
 }
// frame: record into the caller's frame command buffer (retained reloads, no submit or wait); staging is retired until
// that frame completed. Otherwise own submissions, each waited, staged through one reusable buffer (`ring`); `pool`
// (full scene loads) sub-allocates the destinations.
struct Staged {VkBuffer handle=VK_NULL_HANDLE;VkDeviceSize offset=0;};
struct UploadBatch {
    VulkanRenderer& renderer;
    MemoryPool* pool=nullptr;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    std::vector<Buffer> staging;
    Buffer ring;
    VkDeviceSize bytes=0,total_bytes=0;
    std::size_t submissions=0;
    bool recording=false,pending=false,frame=false;
    explicit UploadBatch(VulkanRenderer& r,VkCommandBuffer into=VK_NULL_HANDLE,MemoryPool* destinations=nullptr)
        :renderer(r),pool(destinations),command(into),recording(into!=VK_NULL_HANDLE),frame(into!=VK_NULL_HANDLE){}
    ~UploadBatch() {
        if(frame){for(auto& stage:staging)renderer.retire(stage);return;}
        if(pending)renderer.drain_fence(fence);
        if(command)vkFreeCommandBuffers(renderer.device_,renderer.pool_,1,&command);
        if(fence)vkDestroyFence(renderer.device_,fence,nullptr);
        for(auto& stage:staging)renderer.destroy(stage);
        renderer.destroy(ring);
    }
    void flush() {
        if(frame) {
            if(!staging.empty())++submissions;
            for(auto& stage:staging)renderer.retire(stage);
            staging.clear();bytes=0;return;
        }
        if(!recording)return;
        checked(vkEndCommandBuffer(command),"end scene upload batch");
        recording=false;
        checked(vkResetFences(renderer.device_,1,&fence),"reset scene upload fence");
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        checked(renderer.queue_submit(submit,fence),"submit scene upload batch");
        pending=true;
        renderer.wait_fence(fence,"wait scene upload batch");
        pending=false;++submissions;
        checked(vkResetCommandBuffer(command,0),"reset scene upload batch");
        for(auto& stage:staging)renderer.destroy(stage);
        staging.clear();bytes=0;
    }
    Staged stage(const void* data,VkDeviceSize size) {
        // Bound aggregate staging to one eighth of the asset cap; a larger single asset runs alone. Offsets stay 16-byte
        // aligned (BC block and texel copy offsets).
        renderer.progress();
        constexpr VkDeviceSize chunk_bytes=assets::max_entry_bytes/8;
        if(bytes&&(size>chunk_bytes||((bytes+15)&~VkDeviceSize(15))>chunk_bytes-size))flush();
        if(!command) {
            VkCommandBufferAllocateInfo allocate{};allocate.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocate.commandPool=renderer.pool_;allocate.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;allocate.commandBufferCount=1;
            checked(vkAllocateCommandBuffers(renderer.device_,&allocate,&command),"allocate scene upload batch");
            VkFenceCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            checked(vkCreateFence(renderer.device_,&create,nullptr,&fence),"create scene upload fence");
        }
        if(!recording) {
            VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            checked(vkBeginCommandBuffer(command,&begin),"begin scene upload batch");recording=true;
        }
        renderer.progress();
        total_bytes+=size;
        if(!frame&&size<=chunk_bytes) { // flush() waits for the copies before the ring is rewritten
            if(!ring.handle)ring=renderer.buffer(chunk_bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            const VkDeviceSize offset=(bytes+15)&~VkDeviceSize(15);
            renderer.copy_staging(static_cast<std::byte*>(ring.mapped)+offset,data,static_cast<std::size_t>(size));
            bytes=offset+size;
            return {ring.handle,offset};
        }
        auto next=renderer.buffer(size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {staging.push_back(next);}catch(...) {renderer.destroy(next);throw;}
        renderer.copy_staging(next.mapped,data,static_cast<std::size_t>(size));
        bytes+=size;
        return {next.handle,0};
    }
};
static VkFormat texture_format(assets::DdsFormat format) {
    switch(format) {
    case assets::DdsFormat::bc1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; // DXT1 1-bit alpha: the RGBA variant
    case assets::DdsFormat::bc2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case assets::DdsFormat::bc3: return VK_FORMAT_BC3_UNORM_BLOCK;
    default: return VK_FORMAT_R8G8B8A8_UNORM;
    }
}
// pixels: the mip chain in `format` (RGBA8 texels or authored BC blocks), tightly packed per level.
Image upload(std::uint32_t w,std::uint32_t h,const std::uint8_t* pixels,
             std::span<const assets::DdsMip> mips={},std::size_t bytes=0,UploadBatch* batch=nullptr,
             assets::DdsFormat format=assets::DdsFormat::rgba8) {
    if(!w||!h||std::uint64_t(w)*h>134217728)throw std::runtime_error("invalid Vulkan texture dimensions");
    const auto levels=mips.empty()?1u:static_cast<std::uint32_t>(mips.size());
    if(levels>32)throw std::runtime_error("invalid original mip count");
    if(!bytes)bytes=static_cast<std::size_t>(assets::dds_mip_bytes(format,w,h));
    if(bytes>assets::max_entry_bytes)throw std::runtime_error("decoded original mip chain exceeds upload limit");
    std::array<VkBufferImageCopy,32> copies{};
    std::size_t expected_offset=0;
    for(std::uint32_t level=0;level<levels;++level) {
        progress();
        const auto width=std::max(1u,w>>level),height=std::max(1u,h>>level);
        const auto size=static_cast<std::size_t>(assets::dds_mip_bytes(format,width,height));
        if(!mips.empty()&&(mips[level].width!=width||mips[level].height!=height||
           mips[level].offset!=expected_offset||mips[level].size!=size))
            throw std::runtime_error("invalid decoded original mip descriptor");
        if(size>bytes||expected_offset>bytes-size)throw std::runtime_error("truncated original mip chain");
        copies[level].bufferOffset=expected_offset;
        copies[level].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,1};
        copies[level].imageExtent={width,height,1};expected_offset+=size;
    }
    if(expected_offset!=bytes)throw std::runtime_error("unexpected original mip chain trailing bytes");
    Buffer staging;
    Staged source;
    Image i;
    try {
        i=image(w,h,texture_format(format),VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,levels,batch?batch->pool:nullptr);
        if(batch)source=batch->stage(pixels,bytes);
        else {
            staging=buffer(bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            copy_staging(staging.mapped,pixels,bytes);
            source={staging.handle,0};
        }
        for(std::uint32_t level=0;level<levels;++level)copies[level].bufferOffset+=source.offset;
        auto record=[&](VkCommandBuffer c) {
            barrier(c,i.handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_IMAGE_ASPECT_COLOR_BIT,levels);
            vkCmdCopyBufferToImage(c,source.handle,i.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,levels,copies.data());
            barrier(c,i.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_IMAGE_ASPECT_COLOR_BIT,levels);
        };
        if(batch)record(batch->command);else immediate(record);
    } catch(...) {if(!batch)destroy(staging);destroy(i);throw;}
    if(!batch)destroy(staging);
    return i;
}
// Scene/particle textures: authored BC blocks stay compressed when the device samples BC (bc_supported_); otherwise they
// decode to RGBA8 exactly like the CPU decoder.
Image upload_texture(const Texture& t,UploadBatch* batch=nullptr) {
    if(t.data.empty())throw std::runtime_error("invalid decoded original texture size");
    if(t.format==assets::DdsFormat::rgba8||bc_supported_)return upload(t.width,t.height,t.data.data(),t.mips,t.data.size(),batch,t.format);
    const auto rgba=assets::decompress_dds({t.width,t.height,t.data,t.mips,t.format});
    return upload(rgba.width,rgba.height,rgba.rgba.data(),rgba.mips,rgba.rgba.size(),batch);
}
Buffer upload_buffer(const void* data,VkDeviceSize bytes,VkBufferUsageFlags usage,UploadBatch& batch) {
    auto result=buffer(bytes,usage|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,batch.pool);
    Staged stage;
    try {stage=batch.stage(data,bytes);}catch(...) {destroy(result);throw;}
    VkBufferCopy copy{stage.offset,0,bytes};vkCmdCopyBuffer(batch.command,stage.handle,result.handle,1,&copy);
    VkBufferMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT|VK_ACCESS_INDEX_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=result.handle;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(batch.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
        0,0,nullptr,1,&barrier,0,nullptr);
    return result;
}
 std::vector<std::uint32_t> shader_bytes(const char* name){std::vector<std::filesystem::path> roots{IKV_SHADER_DIR};if(const char* base=SDL_GetBasePath()){roots.emplace_back(std::filesystem::path(base)/"shaders/vulkan");roots.emplace_back(std::filesystem::path(base)/"../share/ikv/shaders/vulkan");}for(const auto& root:roots){std::ifstream f(root/(std::string(name)+".spv"),std::ios::binary|std::ios::ate);if(!f)continue;auto length=static_cast<std::streamoff>(f.tellg());if(length<20||length>16*1024*1024||length%4!=0)throw std::runtime_error("invalid SPIR-V file");std::vector<std::uint32_t> words(static_cast<std::size_t>(length)/4);f.seekg(0);f.read(reinterpret_cast<char*>(words.data()),length);if(!f||words[0]!=0x07230203)throw std::runtime_error("invalid SPIR-V magic/read");return words;}throw std::runtime_error(std::string("compiled Vulkan shader missing: ")+name+".spv");}
 VkShaderModule shader(const char* name){auto words=shader_bytes(name);VkShaderModuleCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;c.codeSize=words.size()*4;c.pCode=words.data();VkShaderModule module;checked(vkCreateShaderModule(device_,&c,nullptr,&module),"create shader module");return module;}
VkPipeline pipeline(const char* vert,const char* frag,VkRenderPass pass,bool mesh,bool alpha,
                    bool depth,bool shadow,bool double_sided=false,bool depth_write=true,std::optional<ParticleBlend> particle={},
                    bool contour=false,bool glow_composite=false) {
     progress();
     VkShaderModule v=shader(vert),f=VK_NULL_HANDLE;VkPipeline result=VK_NULL_HANDLE;
     try {
         f=shader(frag);VkPipelineShaderStageCreateInfo stages[2]{};
         for(auto& s:stages)s.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
         stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=v;stages[0].pName="main";
         stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=f;stages[1].pName="main";
        // scene.vert binding 1 = the previous frame's skinned pose (static meshes bind binding 0's buffer again).
        const bool previous_pose=std::strcmp(vert,"scene.vert")==0;
        const VkVertexInputBindingDescription bindings[2]{{0,static_cast<std::uint32_t>(particle?sizeof(ParticleVertex):sizeof(Vertex)),VK_VERTEX_INPUT_RATE_VERTEX},
            {1,sizeof(Vertex),VK_VERTEX_INPUT_RATE_VERTEX}};
        VkVertexInputAttributeDescription attributes[8]{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,x)},{1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,nx)},{2,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,u)},{3,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,ambient)},{4,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,tangent)},{5,0,VK_FORMAT_R32G32_SFLOAT,offsetof(Vertex,uv1)},{6,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(Vertex,color)},{7,1,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex,x)}};
        const VkVertexInputAttributeDescription particle_attributes[3]{{0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(ParticleVertex,x)},
            {1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ParticleVertex,u)},{2,0,VK_FORMAT_R32G32B32A32_SFLOAT,offsetof(ParticleVertex,color)}};
        VkPipelineVertexInputStateCreateInfo input{};input.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if(mesh) {
            input.vertexBindingDescriptionCount=previous_pose?2:1;input.pVertexBindingDescriptions=bindings;
            input.vertexAttributeDescriptionCount=particle?3:previous_pose?8:7;input.pVertexAttributeDescriptions=particle?particle_attributes:attributes;
        }
         VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
         assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
         VkPipelineViewportStateCreateInfo viewport{};viewport.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
         viewport.viewportCount=viewport.scissorCount=1;
         VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
         raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=contour?VK_CULL_MODE_FRONT_BIT:mesh&&!double_sided?VK_CULL_MODE_BACK_BIT:VK_CULL_MODE_NONE;
         // Source ADF triangles keep D3D clockwise front faces; the shared Y-flipped projection preserves GL's choice.
         raster.frontFace=VK_FRONT_FACE_CLOCKWISE;raster.lineWidth=1;raster.depthBiasEnable=shadow;
         raster.depthBiasConstantFactor=shadow?1.25f:0;raster.depthBiasSlopeFactor=shadow?1.75f:0;
         VkPipelineMultisampleStateCreateInfo samples{};samples.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
         samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
         VkPipelineDepthStencilStateCreateInfo ds{};ds.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
         ds.depthTestEnable=depth;ds.depthWriteEnable=depth&&!alpha&&depth_write;ds.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
         VkPipelineColorBlendAttachmentState blend{};
         blend.colorWriteMask=0xf;blend.blendEnable=alpha;
         blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
         blend.colorBlendOp=VK_BLEND_OP_ADD;blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.alphaBlendOp=VK_BLEND_OP_ADD;
        if(particle) { // Original 0049a780 SetBlend(enable,src,dst,op) per BlendType.
            const bool sum=*particle==ParticleBlend::add||*particle==ParticleBlend::subtract;
            blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=sum?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;
            blend.colorBlendOp=blend.alphaBlendOp=*particle==ParticleBlend::subtract?VK_BLEND_OP_REVERSE_SUBTRACT:
                *particle==ParticleBlend::min?VK_BLEND_OP_MIN:*particle==ParticleBlend::max?VK_BLEND_OP_MAX:VK_BLEND_OP_ADD;
        }
        // 0046fdb0 @4716a2: glow composite SRCBLEND SRCCOLOR, DESTBLEND ONE (frame += src*src).
        if(glow_composite){blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_COLOR;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE;}
        // MRT locations beyond 0 (see scene_colors_): geometry guides/histories never alpha-blend; overlays leave them.
        std::array<VkPipelineColorBlendAttachmentState,8> blends{};blends[0]=blend;
        for(std::size_t i=1;i<blends.size();++i)blends[i].colorWriteMask=particle||contour?0:0xf;
        if(pass==scene_pass_||pass==after_pass_) {
            // Blended scene meshes (depth_write: not sky layers) are drawn back to front without depth writes, so the last
            // fragment is the one shown: like opaque draws they overwrite motion, the RT guide (lighting.glsl) and reactive.
            const bool surface=!alpha||(mesh&&!particle&&!contour&&depth_write);
            // [2] motion: sky layers/particle/contour draws keep the motion of what they cover.
            blends[2].colorWriteMask=surface?VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT:0;
            // [5] FSR reactive: opaque geometry writes 0 (ZERO*src+ZERO*dst, also over earlier translucent sky layers),
            // blended surfaces write their own value, other translucent draws keep the maximum coverage (MAX ignores
            // the factors).
            const VkBlendFactor reactive=alpha?VK_BLEND_FACTOR_ONE:VK_BLEND_FACTOR_ZERO;
            const VkBlendOp reactive_op=alpha?VK_BLEND_OP_MAX:VK_BLEND_OP_ADD;
            blends[5]={!(alpha&&surface),reactive,reactive,reactive_op,reactive,reactive,reactive_op,
                mesh?VkColorComponentFlags{VK_COLOR_COMPONENT_R_BIT}:0u};
        }
        // Without independentBlend (enabled only for denoise/FSR) every location past 0 is VK_ATTACHMENT_UNUSED.
        if(!(denoise_enabled_||fsr_supported_))std::fill(blends.begin()+1,blends.end(),blends[0]);
         VkPipelineColorBlendStateCreateInfo blending{};blending.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
         blending.attachmentCount=shadow?0:static_cast<std::uint32_t>(colors_of(pass).size());
         blending.pAttachments=shadow?nullptr:blends.data();
         VkDynamicState dynamic_states[2]{VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
         VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
         dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamic_states;
         VkGraphicsPipelineCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
         create.stageCount=2;create.pStages=stages;create.pVertexInputState=&input;create.pInputAssemblyState=&assembly;
         create.pViewportState=&viewport;create.pRasterizationState=&raster;create.pMultisampleState=&samples;
         create.pDepthStencilState=&ds;create.pColorBlendState=&blending;create.pDynamicState=&dynamic;
         create.layout=pipeline_layout_;create.renderPass=pass;
         checked(vkCreateGraphicsPipelines(device_,cache_,1,&create,nullptr,&result),"create graphics pipeline");
         progress();
     } catch(...) {
         if(result)vkDestroyPipeline(device_,result,nullptr);
         vkDestroyShaderModule(device_,v,nullptr);if(f)vkDestroyShaderModule(device_,f,nullptr);throw;
     }
     vkDestroyShaderModule(device_,v,nullptr);vkDestroyShaderModule(device_,f,nullptr);return result;
 }
// colors: one format per fragment output location, VK_FORMAT_UNDEFINED = VK_ATTACHMENT_UNUSED; attachments are the defined
// colours in location order, then depth. load: continue a finished scene target (colour SHADER_READ_ONLY, read-only
// depth) for the AfterHDR particle pass.
VkRenderPass render_pass(std::span<const VkFormat> colors,VkImageLayout final,bool depth,bool load=false) {
     std::array<VkAttachmentDescription,9> attachments{};
     std::array<VkAttachmentReference,8> refs{};
     VkAttachmentReference depth_ref{};
     if(colors.size()>refs.size())throw std::logic_error("too many colour locations");
     std::uint32_t count=0;
     for(std::size_t i=0;i<colors.size();++i) {
         if(colors[i]==VK_FORMAT_UNDEFINED){refs[i]={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_UNDEFINED};continue;}
         auto& a=attachments[count];
         a.format=colors[i];a.samples=VK_SAMPLE_COUNT_1_BIT;
         a.loadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
         a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
         a.initialLayout=load?final:VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=final;
         refs[i]={count++,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
     }
     if(depth) {
         auto& a=attachments[count];a.format=VK_FORMAT_D32_SFLOAT;a.samples=VK_SAMPLE_COUNT_1_BIT;
         a.loadOp=load?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_CLEAR;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
         a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
         a.initialLayout=load?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;a.finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
         depth_ref={count++,load?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
     }
     VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
     subpass.colorAttachmentCount=static_cast<std::uint32_t>(colors.size());subpass.pColorAttachments=colors.empty()?nullptr:refs.data();
     subpass.pDepthStencilAttachment=depth?&depth_ref:nullptr;
     // Compute stages: the FSR 3.1.4 dispatch reads finished targets and its reads precede the next overwrite.
     VkSubpassDependency dependencies[2]{};
     dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
     dependencies[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
     dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
     dependencies[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
     dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
     if(load) { // Earlier scene/temporal attachment writes are blended against and depth-tested.
         dependencies[0].srcStageMask|=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
         dependencies[0].srcAccessMask|=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
         dependencies[0].dstAccessMask|=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
     }
     dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
     dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
     dependencies[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
     dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
     dependencies[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
     VkRenderPassCreateInfo create{};create.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
     create.attachmentCount=count;create.pAttachments=attachments.data();create.subpassCount=1;
     create.pSubpasses=&subpass;create.dependencyCount=2;create.pDependencies=dependencies;
     VkRenderPass result;checked(vkCreateRenderPass(device_,&create,nullptr,&result),"create render pass");return result;
 }
 VkFramebuffer framebuffer(VkRenderPass pass,std::span<const VkImageView> views,VkExtent2D size){VkFramebufferCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;c.renderPass=pass;c.attachmentCount=static_cast<std::uint32_t>(views.size());c.pAttachments=views.data();c.width=size.width;c.height=size.height;c.layers=1;VkFramebuffer fb;checked(vkCreateFramebuffer(device_,&c,nullptr,&fb),"create framebuffer");return fb;}
 void init() {
     validate_camera(options_.camera);
     if(!std::isfinite(options_.render_scale)||options_.render_scale<.25f||options_.render_scale>1||
        !std::isfinite(options_.exposure)||options_.exposure<=0)
         throw std::invalid_argument("invalid Vulkan render scale/exposure");
     if(!std::isfinite(options_.fsr_sharpness)||options_.fsr_sharpness<0||options_.fsr_sharpness>1)
         throw std::invalid_argument("FSR sharpness requires [0,1]");
     Uint32 count=0;auto extensions=SDL_Vulkan_GetInstanceExtensions(&count);
     if(!extensions)throw std::runtime_error(SDL_GetError());
     VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
     app.pApplicationName="IKV native client";app.apiVersion=VK_API_VERSION_1_2;
     // SDL's surface extensions, plus VK_KHR_get_surface_capabilities2 when available: VK_EXT_present_timing (frame
     // generation display-time measurement) depends on it.
     std::vector<const char*> instance_extensions(extensions,extensions+count);
     std::uint32_t available_count=0;checked(vkEnumerateInstanceExtensionProperties(nullptr,&available_count,nullptr),"instance extensions");
     std::vector<VkExtensionProperties> available_instance(available_count);
     checked(vkEnumerateInstanceExtensionProperties(nullptr,&available_count,available_instance.data()),"instance extensions");
     const bool surface_capabilities2=std::any_of(available_instance.begin(),available_instance.end(),
         [](const auto& e){return !std::strcmp(e.extensionName,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);});
     if(surface_capabilities2&&std::none_of(instance_extensions.begin(),instance_extensions.end(),
            [](const char* e){return !std::strcmp(e,VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);}))
         instance_extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
     VkInstanceCreateInfo ic{};ic.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
     ic.pApplicationInfo=&app;ic.enabledExtensionCount=static_cast<std::uint32_t>(instance_extensions.size());
     ic.ppEnabledExtensionNames=instance_extensions.data();
     checked(vkCreateInstance(&ic,nullptr,&instance_),"create Vulkan instance");
     if(!SDL_Vulkan_CreateSurface(window_,instance_,nullptr,&surface_))throw std::runtime_error(SDL_GetError());
     std::uint32_t n=0;checked(vkEnumeratePhysicalDevices(instance_,&n,nullptr),"enumerate GPUs");
     std::vector<VkPhysicalDevice> devices(n);
     checked(vkEnumeratePhysicalDevices(instance_,&n,devices.data()),"enumerate GPUs");
     int best=-1;
     for(auto p:devices) {
         progress();
         VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(p,&props);
         if(props.apiVersion<VK_API_VERSION_1_2)continue;
         std::uint32_t queues=0;vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,nullptr);
         std::vector<VkQueueFamilyProperties> q(queues);
         vkGetPhysicalDeviceQueueFamilyProperties(p,&queues,q.data());
         const auto features=query_rt_features(p);
         for(std::uint32_t j=0;j<queues;++j) {
             VkBool32 present=VK_FALSE;
             checked(vkGetPhysicalDeviceSurfaceSupportKHR(p,j,surface_,&present),"surface support");
             if(!(q[j].queueFlags&VK_QUEUE_GRAPHICS_BIT)||!present)continue;
             const bool compute=(q[j].queueFlags&VK_QUEUE_COMPUTE_BIT)!=0;
             if(options_.ray_tracing==RayTracing::required&&(!features.supported()||!compute)) {
                 std::cerr<<"Vulkan RT candidate="<<props.deviceName
                     <<" accelerationStructure(feature+extension)="<<features.acceleration_structure
                     <<" rayQuery(feature+extension)="<<features.ray_query
                     <<" bufferDeviceAddress="<<features.buffer_device_address<<" computeQueue="<<compute<<'\n';
                 continue;
             }
             int score=props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?100:10;
             if(features.supported()&&compute)score+=50;
             if(score>best){best=score;physical_=p;family_=j;}
         }
     }
     if(!physical_)throw std::runtime_error("no Vulkan1.2 graphics/present device satisfies requested capabilities");
     const auto features=query_rt_features(physical_);
     // Physical RT only traces in enhanced lighting: with original appearance it would build/refresh AS for nothing.
     rt_enabled_=options_.ray_tracing!=RayTracing::off&&features.supported()&&compute_queue()&&
         (options_.rt_style==RtStyle::faithful||!options_.original_appearance);
     original_lighting_=options_.original_appearance||(rt_enabled_&&options_.rt_style==RtStyle::faithful);
     const char* denoise=SDL_getenv("IKV_RT_DENOISE");
     denoise_enabled_=rt_enabled_&&!original_lighting_&&(!denoise||std::strcmp(denoise,"0"));
     VkPhysicalDeviceFeatures available{};vkGetPhysicalDeviceFeatures(physical_,&available);
     if(denoise_enabled_&&!available.independentBlend)
         throw std::runtime_error("Vulkan guided RT denoise requires independentBlend");
     VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical_,&props);
     if(props.limits.maxPushConstantsSize<sizeof(Draw))throw std::runtime_error("Vulkan device push constants are smaller than 192 bytes");
     // FSR's features are enabled whenever supported, so Ayarlar can switch the upscaler without a new device.
     const char* fsr_missing=available.independentBlend?nullptr:"independentBlend unavailable";
     fsr_supported_=!fsr_missing&&Fsr3Upscaler::supported(physical_,&fsr_missing);
     std::cerr<<"Vulkan device="<<props.deviceName<<" accelerationStructure="<<features.acceleration_structure
         <<" rayQuery="<<features.ray_query<<" bufferDeviceAddress="<<features.buffer_device_address
         <<" hardwareRT="<<rt_enabled_<<" fsr3.1.4="<<fsr_supported_<<(fsr_missing?" fsrUnsupported=":"")<<(fsr_missing?fsr_missing:"")<<'\n';
     float priority=1;
     VkDeviceQueueCreateInfo qc{};qc.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
     qc.queueFamilyIndex=family_;qc.queueCount=1;qc.pQueuePriorities=&priority;
     // Frame generation measures display times with VK_EXT_present_timing (first pixel out, CLOCK_MONOTONIC) when the
     // device and surface support it; otherwise it reports present-call cadence.
     VkPhysicalDevicePresentTimingFeaturesEXT timing_features{};timing_features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT;
     VkPhysicalDevicePresentId2FeaturesKHR id_features{};id_features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR;
     id_features.pNext=&timing_features;
     const auto surface_caps2=reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
         vkGetInstanceProcAddr(instance_,"vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
     // Wayland only: Mesa 26.2's X11 present-timing path raised SIGFPE inside RADV on Xvfb (no refresh rate).
     const char* video=SDL_GetCurrentVideoDriver();
     if(fsr_supported_&&surface_capabilities2&&surface_caps2&&video&&!std::strcmp(video,"wayland")) {
         std::uint32_t extension_count=0;checked(vkEnumerateDeviceExtensionProperties(physical_,nullptr,&extension_count,nullptr),"device extensions");
         std::vector<VkExtensionProperties> available_extensions(extension_count);
         checked(vkEnumerateDeviceExtensionProperties(physical_,nullptr,&extension_count,available_extensions.data()),"device extensions");
         const auto has=[&](const char* name) {
             return std::any_of(available_extensions.begin(),available_extensions.end(),[&](const auto& e){return !std::strcmp(e.extensionName,name);});
         };
         if(has(VK_KHR_PRESENT_ID_2_EXTENSION_NAME)&&has(VK_EXT_PRESENT_TIMING_EXTENSION_NAME)&&has(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) {
             VkPhysicalDeviceFeatures2 query{};query.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;query.pNext=&id_features;
             vkGetPhysicalDeviceFeatures2(physical_,&query);
             VkPresentTimingSurfaceCapabilitiesEXT timing_caps{};timing_caps.sType=VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT;
             VkSurfaceCapabilities2KHR caps2{};caps2.sType=VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;caps2.pNext=&timing_caps;
             VkPhysicalDeviceSurfaceInfo2KHR surface_info{};surface_info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
             surface_info.surface=surface_;
             present_timing_=id_features.presentId2&&timing_features.presentTiming&&
                 surface_caps2(physical_,&surface_info,&caps2)==VK_SUCCESS&&timing_caps.presentTimingSupported&&
                 (timing_caps.presentStageQueries&VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
             id_features.presentId2=timing_features.presentTiming=present_timing_?VK_TRUE:VK_FALSE;
             timing_features.presentAtAbsoluteTime=timing_features.presentAtRelativeTime=VK_FALSE;
         }
     }
     // A fixed array: vector push_back growth from one element trips a GCC -O2 -Warray-bounds false positive.
     std::array<const char*,7> de{VK_KHR_SWAPCHAIN_EXTENSION_NAME};std::uint32_t de_count=1;
     if(rt_enabled_)for(const char* e:{VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
                                        VK_KHR_RAY_QUERY_EXTENSION_NAME})de[de_count++]=e;
     if(present_timing_)for(const char* e:{VK_KHR_PRESENT_ID_2_EXTENSION_NAME,VK_EXT_PRESENT_TIMING_EXTENSION_NAME,
                                           VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME})de[de_count++]=e;
     VkPhysicalDeviceBufferDeviceAddressFeatures bda{};bda.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
     VkPhysicalDeviceAccelerationStructureFeaturesKHR as{};as.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
     VkPhysicalDeviceRayQueryFeaturesKHR rq{};rq.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
     if(rt_enabled_) {
         bda.bufferDeviceAddress=VK_TRUE;bda.pNext=&as;as.accelerationStructure=VK_TRUE;
         as.pNext=&rq;rq.rayQuery=VK_TRUE;
     }
     VkDeviceCreateInfo dc{};dc.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
     dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qc;
     dc.enabledExtensionCount=de_count;dc.ppEnabledExtensionNames=de.data();
     timing_features.pNext=rt_enabled_?&bda:nullptr;
     dc.pNext=present_timing_?static_cast<void*>(&id_features):rt_enabled_?static_cast<void*>(&bda):nullptr;
     VkPhysicalDeviceFeatures enabled{};enabled.independentBlend=denoise_enabled_||fsr_supported_;
     // Authored DXT1/3/5 stay compressed on the GPU when BC1/2/3 sample with linear filtering (upload_texture).
     bc_supported_=available.textureCompressionBC;
     for(const auto format:{VK_FORMAT_BC1_RGBA_UNORM_BLOCK,VK_FORMAT_BC2_UNORM_BLOCK,VK_FORMAT_BC3_UNORM_BLOCK}) {
         VkFormatProperties bc{};vkGetPhysicalDeviceFormatProperties(physical_,format,&bc);
         constexpr VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
         bc_supported_=bc_supported_&&(bc.optimalTilingFeatures&needed)==needed;
     }
     enabled.textureCompressionBC=bc_supported_;
     // Original 0046b040 sets D3DSAMP_MAXANISOTROPY (launcher default 4x) on the texture stages; 16 measured no worse in
     // motion (wave-12 Textures report §2).
     enabled.samplerAnisotropy=available.samplerAnisotropy;
     const float anisotropy=available.samplerAnisotropy?std::min(16.f,props.limits.maxSamplerAnisotropy):1.f;
     if(fsr_supported_)Fsr3Upscaler::enable_features(physical_,enabled);
     dc.pEnabledFeatures=&enabled;
     checked(vkCreateDevice(physical_,&dc,nullptr,&device_),"create Vulkan device");
     vkGetDeviceQueue(device_,family_,0,&queue_);
     if(present_timing_) {
         past_timing_=reinterpret_cast<PFN_vkGetPastPresentationTimingEXT>(vkGetDeviceProcAddr(device_,"vkGetPastPresentationTimingEXT"));
         timing_queue_=reinterpret_cast<PFN_vkSetSwapchainPresentTimingQueueSizeEXT>(vkGetDeviceProcAddr(device_,"vkSetSwapchainPresentTimingQueueSizeEXT"));
         time_domains_=reinterpret_cast<PFN_vkGetSwapchainTimeDomainPropertiesEXT>(vkGetDeviceProcAddr(device_,"vkGetSwapchainTimeDomainPropertiesEXT"));
         calibrated_=reinterpret_cast<PFN_vkGetCalibratedTimestampsKHR>(vkGetDeviceProcAddr(device_,"vkGetCalibratedTimestampsKHR"));
         present_timing_=past_timing_&&timing_queue_&&time_domains_;
     }
     VkCommandPoolCreateInfo pc{};pc.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
     pc.queueFamilyIndex=family_;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
     checked(vkCreateCommandPool(device_,&pc,nullptr,&pool_),"command pool");
     VkCommandBufferAllocateInfo ca{};ca.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
     ca.commandPool=pool_;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
     VkFenceCreateInfo fc{};fc.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;fc.flags=VK_FENCE_CREATE_SIGNALED_BIT;
     VkSemaphoreCreateInfo sc{};sc.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
     for(auto& slot:slots_) {
         checked(vkAllocateCommandBuffers(device_,&ca,&slot.command),"frame command");
         checked(vkCreateFence(device_,&fc,nullptr,&slot.fence),"frame fence");
         checked(vkCreateSemaphore(device_,&sc,nullptr,&slot.acquire),"acquire semaphore");
     }
     // The present thread's own pool: command pools are single-threaded.
     checked(vkCreateCommandPool(device_,&pc,nullptr,&present_pool_),"present command pool");
     ca.commandPool=present_pool_;ca.commandBufferCount=2;
     checked(vkAllocateCommandBuffers(device_,&ca,present_commands_),"present commands");
     for(unsigned i=0;i<2;++i) {
         checked(vkCreateFence(device_,&fc,nullptr,&present_fences_[i]),"present fence");
         checked(vkCreateSemaphore(device_,&sc,nullptr,&present_acquire_[i]),"present acquire semaphore");
     }
     if(const char* lead=SDL_getenv("IKV_FG_PACING_LEAD_MS")) { // calibration knob for compositor latch timing
         const float value=std::strtof(lead,nullptr);
         if(std::isfinite(value)&&value>=0&&value<=20)pacing_lead_ms_=value;
     }
     VkSamplerCreateInfo sampler{};sampler.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
     // GL parity: original appearance keeps the source's nearest mip (LINEAR_MIPMAP_NEAREST), enhanced blends mips
     // (LINEAR_MIPMAP_LINEAR); hard mip switches popped alpha-tested foliage coverage with distance.
     sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
     sampler.mipmapMode=options_.original_appearance?VK_SAMPLER_MIPMAP_MODE_NEAREST:VK_SAMPLER_MIPMAP_MODE_LINEAR;
     sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;sampler.maxLod=VK_LOD_CLAMP_NONE;
     sampler.anisotropyEnable=anisotropy>1?VK_TRUE:VK_FALSE;sampler.maxAnisotropy=anisotropy;
     checked(vkCreateSampler(device_,&sampler,nullptr,&sampler_),"texture sampler");
     sampler.anisotropyEnable=VK_FALSE;sampler.maxAnisotropy=1; // clamp/shadow samplers read render targets
     sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
     checked(vkCreateSampler(device_,&sampler,nullptr,&linear_clamp_),"linear clamp sampler");
     sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
     checked(vkCreateSampler(device_,&sampler,nullptr,&shadow_sampler_),"shadow sampler");
     std::array<VkDescriptorSetLayoutBinding,12> bindings{};
     bindings[0]={0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
     const std::uint32_t binding_count=denoise_enabled_?12:10;
     for(std::uint32_t i=1;i<binding_count;++i)bindings[i]={i,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
     VkDescriptorSetLayoutCreateInfo lc{};lc.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
     lc.bindingCount=binding_count;lc.pBindings=bindings.data();
     checked(vkCreateDescriptorSetLayout(device_,&lc,nullptr,&set_layout_),"material descriptor layout");
     std::array<VkDescriptorSetLayout,2> layouts{set_layout_,VK_NULL_HANDLE};
     if(rt_enabled_) {
         VkDescriptorSetLayoutBinding rbindings[2]{
             {0,VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
             {1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
         lc.bindingCount=2;lc.pBindings=rbindings;
         checked(vkCreateDescriptorSetLayout(device_,&lc,nullptr,&rt_layout_),"RT descriptor layout");
         layouts[1]=rt_layout_;rt_=std::make_unique<VulkanRtScene>(physical_,device_,queue_,family_,options_.progress);
     }
     VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Draw)};
     VkPipelineLayoutCreateInfo pl{};pl.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
     pl.setLayoutCount=rt_enabled_?2:1;pl.pSetLayouts=layouts.data();pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
     checked(vkCreatePipelineLayout(device_,&pl,nullptr,&pipeline_layout_),"pipeline layout");
     const auto align=std::max<VkDeviceSize>(1,props.limits.minUniformBufferOffsetAlignment);
     uniform_stride_=static_cast<std::uint32_t>((sizeof(Frame)+align-1)/align*align);
     uniform_=buffer(2*VkDeviceSize(uniform_stride_),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
     const std::uint8_t white[4]{255,255,255,255},normal[4]{128,128,255,255},clear[4]{};
     white_=upload(1,1,white);flat_normal_=upload(1,1,normal);overlay_=upload(1,1,clear);
     if(char* pref=SDL_GetPrefPath("ikv","native")) {
         cache_path_=std::filesystem::path(pref)/"vulkan-pipeline.cache";SDL_free(pref);
     }
     std::vector<char> initial;
     std::ifstream file(cache_path_,std::ios::binary|std::ios::ate);
     if(file) {
         const auto size=file.tellg();
         if(size>0&&size<32*1024*1024) {
             initial.resize(static_cast<std::size_t>(size));file.seekg(0);file.read(initial.data(),size);
             if(!file)initial.clear();
         }
     }
     validate_cache(initial,props);
     VkPipelineCacheCreateInfo cc{};cc.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
     cc.initialDataSize=initial.size();cc.pInitialData=initial.data();
     if(vkCreatePipelineCache(device_,&cc,nullptr,&cache_)!=VK_SUCCESS) {
         cc.initialDataSize=0;cc.pInitialData=nullptr;
         checked(vkCreatePipelineCache(device_,&cc,nullptr,&cache_),"pipeline cache");
     }
 }
 void destroy_targets() {
     for(auto* p:{&opaque_,&alpha_,&shadow_pipeline_,&post_pipeline_,&overlay_pipeline_,&hud_pipeline_,
                 &opaque_double_,&alpha_double_,&shadow_double_,&temporal_pipeline_,&contour_pipeline_,&glow_pipeline_,&glow_composite_pipeline_}) {
         if(*p)vkDestroyPipeline(device_,*p,nullptr);
         *p=VK_NULL_HANDLE;
     }
    for(auto& p:sky_pipeline_){if(p)vkDestroyPipeline(device_,p,nullptr);p=VK_NULL_HANDLE;}
    for(auto& p:particle_pipeline_){if(p)vkDestroyPipeline(device_,p,nullptr);p=VK_NULL_HANDLE;}
    for(auto& p:after_particle_pipeline_){if(p)vkDestroyPipeline(device_,p,nullptr);p=VK_NULL_HANDLE;}
     for(auto& f:scene_fb_){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     for(auto& f:temporal_fb_){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     for(auto& slot:atrous_fb_)for(auto& f:slot){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     for(auto& f:after_fb_){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     for(auto& f:glow_fb_){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     if(shadow_fb_)vkDestroyFramebuffer(device_,shadow_fb_,nullptr);
     shadow_fb_=VK_NULL_HANDLE;
     if(post_fb_)vkDestroyFramebuffer(device_,post_fb_,nullptr);
     post_fb_=VK_NULL_HANDLE;
     for(auto& f:hud_fb_){if(f)vkDestroyFramebuffer(device_,f,nullptr);f=VK_NULL_HANDLE;}
     for(auto* p:{&scene_pass_,&shadow_pass_,&post_pass_,&temporal_pass_,&after_pass_,&glow_pass_,&hud_pass_}){if(*p)vkDestroyRenderPass(device_,*p,nullptr);*p=VK_NULL_HANDLE;}
     destroy(hdr_[0]);destroy(hdr_[1]);destroy(raw_hdr_);destroy(guide_[0]);destroy(guide_[1]);destroy(depth_);destroy(shadow_);destroy(output_);destroy(glow_chain_[0]);destroy(glow_chain_[1]);
    for(auto* i:{&generated_[0],&generated_[1],&real_[0],&real_[1]})destroy(*i);
    generating_=false;
    fsr_.reset();destroy(motion_);destroy(reactive_);destroy(fsr_output_);
    for(auto* i:{&rt_diffuse_,&rt_reflection_,&diffuse_history_[0],&diffuse_history_[1],&reflection_history_[0],&reflection_history_[1]})destroy(*i);
    for(auto& i:atrous_)destroy(i);
     for(auto s:present_semaphores_)vkDestroySemaphore(device_,s,nullptr);
     present_semaphores_.clear();swap_images_.clear();
     if(swapchain_)vkDestroySwapchainKHR(device_,swapchain_,nullptr);
     swapchain_=VK_NULL_HANDLE;timed_swapchain_=false;
 }
 bool create_targets() {
     int w=0,h=0;if(!SDL_GetWindowSizeInPixels(window_,&w,&h))throw std::runtime_error(SDL_GetError());
     if(w<=0||h<=0)return false;
     wait_idle("resize wait");destroy_targets();
     VkSurfaceCapabilitiesKHR caps;checked(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_,surface_,&caps),"surface capabilities");
     if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))throw std::runtime_error("Vulkan surface does not support transfer destination");
     extent_=caps.currentExtent;
     if(extent_.width==UINT32_MAX) {
         extent_.width=std::clamp(static_cast<std::uint32_t>(w),caps.minImageExtent.width,caps.maxImageExtent.width);
         extent_.height=std::clamp(static_cast<std::uint32_t>(h),caps.minImageExtent.height,caps.maxImageExtent.height);
     }
     render_extent_={std::max(1u,static_cast<std::uint32_t>(float(extent_.width)*options_.render_scale)),
         std::max(1u,static_cast<std::uint32_t>(float(extent_.height)*options_.render_scale))};
     std::uint32_t n=0;checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_,surface_,&n,nullptr),"surface formats");
     std::vector<VkSurfaceFormatKHR> formats(n);
     checked(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_,surface_,&n,formats.data()),"surface formats");
     auto chosen=std::find_if(formats.begin(),formats.end(),[](auto f){return f.format==VK_FORMAT_B8G8R8A8_UNORM||f.format==VK_FORMAT_R8G8B8A8_UNORM;});
     if(chosen==formats.end())throw std::runtime_error("surface RGBA8/BGRA8 UNORM format unavailable");
     swap_format_=chosen->format;
     if(options_.upscaler==Upscaler::fsr3&&!fsr_supported_)std::cerr<<"Vulkan FSR 3.1.4 requested but unsupported; bilinear render scale\n";
     else if(options_.upscaler==Upscaler::fsr3) {
         // The FSR preset replaces render_scale; a context failure keeps the bilinear path rather than ending the session.
         const auto fsr_extent=Fsr3Upscaler::render_extent(extent_,options_.fsr_quality);
         const char* generation_missing=nullptr;
         const bool generation=options_.frame_generation&&Fsr3Upscaler::frame_generation_supported(physical_,swap_format_,&generation_missing);
         if(options_.frame_generation&&!generation)std::cerr<<"Vulkan FSR 3.1.4 frame generation unsupported ("<<generation_missing<<"); off\n";
         try {
             fsr_=std::make_unique<Fsr3Upscaler>(physical_,device_,fsr_extent,extent_,!original_lighting_,
                 generation?swap_format_:VK_FORMAT_UNDEFINED);
             render_extent_=fsr_extent;fsr_reset_=true;
         } catch(const std::exception& e) {std::cerr<<"Vulkan "<<e.what()<<"; bilinear render scale\n";}
     } else if(options_.frame_generation)std::cerr<<"Vulkan frame generation requires the FSR 3.1.4 upscaler; off\n";
     generating_=fsr_&&fsr_->generates();generation_frame_=0;
     motion_vectors_=fsr_||denoise_enabled_;
     scene_colors_={VK_FORMAT_R16G16B16A16_SFLOAT};after_colors_=scene_colors_;
     if(denoise_enabled_)scene_colors_.push_back(VK_FORMAT_R32G32B32A32_SFLOAT);
    if(motion_vectors_){scene_colors_.resize(std::max<std::size_t>(scene_colors_.size(),3),VK_FORMAT_UNDEFINED);scene_colors_[2]=VK_FORMAT_R16G16_SFLOAT;}
    if(denoise_enabled_){scene_colors_.resize(std::max<std::size_t>(scene_colors_.size(),5),VK_FORMAT_UNDEFINED);scene_colors_[3]=scene_colors_[4]=VK_FORMAT_R16G16B16A16_SFLOAT;}
    temporal_colors_.assign(denoise_enabled_?3:1,VK_FORMAT_R16G16B16A16_SFLOAT); // display, diffuse/reflection history
    // Scene/after-pass fragment shaders declare locations up to 5 (motion, reactive): slots without a target stay
    // VK_ATTACHMENT_UNUSED, so the render pass discards those writes instead of leaving them without an attachment.
    scene_colors_.resize(std::max<std::size_t>(scene_colors_.size(),6),VK_FORMAT_UNDEFINED);after_colors_.resize(6,VK_FORMAT_UNDEFINED);
    if(fsr_)scene_colors_[5]=after_colors_[5]=VK_FORMAT_R8_UNORM;
     std::uint32_t images=caps.minImageCount+1;if(caps.maxImageCount)images=std::min(images,caps.maxImageCount);
     VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;
     if(!options_.vsync) {
         std::uint32_t count=0;checked(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_,surface_,&count,nullptr),"present modes");
         std::vector<VkPresentModeKHR> modes(count);
         checked(vkGetPhysicalDeviceSurfacePresentModesKHR(physical_,surface_,&count,modes.data()),"present modes");
         if(std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end())mode=VK_PRESENT_MODE_MAILBOX_KHR;
     }
     VkSwapchainCreateInfoKHR create{};create.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
     create.surface=surface_;create.minImageCount=images;create.imageFormat=chosen->format;create.imageColorSpace=chosen->colorSpace;
     create.imageExtent=extent_;create.imageArrayLayers=1;create.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
     create.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;create.preTransform=caps.currentTransform;
     create.compositeAlpha=static_cast<VkCompositeAlphaFlagBitsKHR>(caps.supportedCompositeAlpha&(~caps.supportedCompositeAlpha+1));
     create.presentMode=mode;create.clipped=VK_TRUE;
     // Frame generation swapchains report display times (VK_EXT_present_timing, present_id2 identifies presents).
     if(generating_&&present_timing_)create.flags=VK_SWAPCHAIN_CREATE_PRESENT_TIMING_BIT_EXT|VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR;
     checked(vkCreateSwapchainKHR(device_,&create,nullptr,&swapchain_),"create swapchain");
     if(create.flags) {
         timed_swapchain_=timing_queue_(device_,swapchain_,64)==VK_SUCCESS;
         VkSwapchainTimeDomainPropertiesEXT domains{};domains.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT;
         timed_swapchain_=timed_swapchain_&&time_domains_(device_,swapchain_,&domains,nullptr)==VK_SUCCESS&&domains.timeDomainCount;
         std::vector<VkTimeDomainKHR> kinds(domains.timeDomainCount);std::vector<std::uint64_t> ids(domains.timeDomainCount);
         domains.pTimeDomains=kinds.data();domains.pTimeDomainIds=ids.data();
         timed_swapchain_=timed_swapchain_&&time_domains_(device_,swapchain_,&domains,nullptr)==VK_SUCCESS;
         const auto monotonic=std::find(kinds.begin(),kinds.end(),VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR);
         const std::size_t domain=monotonic!=kinds.end()?std::size_t(monotonic-kinds.begin()):0;
         if(timed_swapchain_)timing_domain_=ids[domain];
         // Offset of that domain to CLOCK_MONOTONIC (steady_clock), for the submit-to-display latency.
         timing_monotonic_=timed_swapchain_&&kinds[domain]==VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;timing_offset_ms_=0;
         if(timed_swapchain_&&!timing_monotonic_&&calibrated_) {
             VkSwapchainCalibratedTimestampInfoEXT local{};local.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT;
             local.swapchain=swapchain_;local.presentStage=VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;local.timeDomainId=timing_domain_;
             VkCalibratedTimestampInfoKHR infos[2]{};
             infos[0].sType=infos[1].sType=VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR;
             infos[0].pNext=&local;infos[0].timeDomain=kinds[domain];infos[1].timeDomain=VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
             std::uint64_t stamps[2]{},deviation=0;
             timing_monotonic_=calibrated_(device_,2,infos,stamps,&deviation)==VK_SUCCESS;
             timing_offset_ms_=(double(stamps[1])-double(stamps[0]))*1e-6;
         }
     }
     checked(vkGetSwapchainImagesKHR(device_,swapchain_,&n,nullptr),"swapchain images");swap_images_.resize(n);
     checked(vkGetSwapchainImagesKHR(device_,swapchain_,&n,swap_images_.data()),"swapchain images");
     present_semaphores_.resize(n,VK_NULL_HANDLE);
     VkSemaphoreCreateInfo sem{};sem.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
     for(auto& s:present_semaphores_)checked(vkCreateSemaphore(device_,&sem,nullptr,&s),"present semaphore");
     const auto hdr_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
     for(auto& i:hdr_)i=image(render_extent_.width,render_extent_.height,VK_FORMAT_R16G16B16A16_SFLOAT,hdr_usage);
     if(original_lighting_) {
         for(auto& i:glow_chain_)i=image(128,128,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
         const VkFormat glow_colors[]{VK_FORMAT_R8G8B8A8_UNORM};
         glow_pass_=render_pass(glow_colors,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,false);
         for(unsigned i=0;i<2;++i){std::array views{glow_chain_[i].view};glow_fb_[i]=framebuffer(glow_pass_,views,{128,128});}
     }
     if(denoise_enabled_) {
         raw_hdr_=image(render_extent_.width,render_extent_.height,VK_FORMAT_R16G16B16A16_SFLOAT,hdr_usage);
        for(auto& i:guide_)i=image(render_extent_.width,render_extent_.height,VK_FORMAT_R32G32B32A32_SFLOAT,hdr_usage);
        for(auto* i:{&rt_diffuse_,&rt_reflection_,&diffuse_history_[0],&diffuse_history_[1],&reflection_history_[0],&reflection_history_[1],
                     &atrous_[0],&atrous_[1],&atrous_[2],&atrous_[3]})
            *i=image(render_extent_.width,render_extent_.height,VK_FORMAT_R16G16B16A16_SFLOAT,hdr_usage);
         temporal_pass_=render_pass(temporal_colors_,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,false);
     }
     if(motion_vectors_)motion_=image(render_extent_.width,render_extent_.height,VK_FORMAT_R16G16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
     if(fsr_) {
         reactive_=image(render_extent_.width,render_extent_.height,VK_FORMAT_R8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
         fsr_output_=image(extent_.width,extent_.height,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
     }
     depth_=image(render_extent_.width,render_extent_.height,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
     shadow_=image(2048,2048,VK_FORMAT_D32_SFLOAT,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
     // Frame generation also samples output_ (HUD-less interpolation source) and copies it into real_.
     constexpr VkImageUsageFlags display_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
     output_=image(extent_.width,extent_.height,swap_format_,display_usage|(generating_?VkImageUsageFlags{VK_IMAGE_USAGE_SAMPLED_BIT}:0u));
     if(generating_)for(unsigned i=0;i<2;++i) {
         generated_[i]=image(extent_.width,extent_.height,swap_format_,display_usage|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT);
         real_[i]=image(extent_.width,extent_.height,swap_format_,VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT);
     }
     scene_pass_=render_pass(scene_colors_,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,true);
     shadow_pass_=render_pass({},VK_IMAGE_LAYOUT_UNDEFINED,true);
     const VkFormat post_colors[]{swap_format_};
     post_pass_=render_pass(post_colors,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,false);
     if(generating_) { // HUD over a finished display image (TRANSFER_SRC_OPTIMAL in and out)
         hud_pass_=render_pass(post_colors,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,false,true);
         for(unsigned i=0;i<3;++i){std::array views{(i<2?generated_[i]:output_).view};hud_fb_[i]=framebuffer(hud_pass_,views,extent_);}
         if(!presenter_.joinable())presenter_=std::thread([this]{present_loop();});
         std::cerr<<"Vulkan FSR 3.1.4 frame generation on presentMode="<<(mode==VK_PRESENT_MODE_MAILBOX_KHR?"mailbox":"fifo")
             <<" displayTiming="<<(timed_swapchain_?(timing_monotonic_?"presentTimingMonotonic":"presentTiming"):"presentCall")
             <<" pacingLeadMs="<<pacing_lead_ms_<<" swapchainImages="<<n<<'\n';
     }
     after_pass_=render_pass(after_colors_,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,true,true);
     for(unsigned i=0;i<2;++i) {
         // Attachment views in scene_colors_ location order, then depth.
         std::vector<VkImageView> views{denoise_enabled_?raw_hdr_.view:hdr_[i].view};
         if(denoise_enabled_)views.push_back(guide_[i].view);
        if(motion_vectors_)views.push_back(motion_.view);
        if(denoise_enabled_){views.push_back(rt_diffuse_.view);views.push_back(rt_reflection_.view);}
         if(fsr_)views.push_back(reactive_.view);
         views.push_back(depth_.view);
         scene_fb_[i]=framebuffer(scene_pass_,views,render_extent_);
        if(denoise_enabled_) {
            std::array temporal_views{hdr_[i].view,diffuse_history_[i].view,reflection_history_[i].view};temporal_fb_[i]=framebuffer(temporal_pass_,temporal_views,render_extent_);
            for(unsigned t=0;t<2;++t){std::array atrous_views{hdr_[i].view,atrous_[2*t].view,atrous_[2*t+1].view};atrous_fb_[i][t]=framebuffer(temporal_pass_,atrous_views,render_extent_);}
        }
         std::vector<VkImageView> after_views{hdr_[i].view};
         if(fsr_)after_views.push_back(reactive_.view);
         after_views.push_back(depth_.view);
         after_fb_[i]=framebuffer(after_pass_,after_views,render_extent_);
     }
     std::array shadow_views{shadow_.view};shadow_fb_=framebuffer(shadow_pass_,shadow_views,{2048,2048});
     std::array post_views{output_.view};post_fb_=framebuffer(post_pass_,post_views,extent_);
     const char* scene_shader=denoise_enabled_?"scene_rt_guided.frag":rt_enabled_?"scene_rt.frag":"scene.frag";
     opaque_=pipeline("scene.vert",scene_shader,scene_pass_,true,false,true,false);
     alpha_=pipeline("scene.vert",scene_shader,scene_pass_,true,true,true,false);
     shadow_pipeline_=pipeline("shadow.vert","shadow.frag",shadow_pass_,true,false,true,true);
     opaque_double_=pipeline("scene.vert",scene_shader,scene_pass_,true,false,true,false,true);
     alpha_double_=pipeline("scene.vert",scene_shader,scene_pass_,true,true,true,false,true);
     shadow_double_=pipeline("shadow.vert","shadow.frag",shadow_pass_,true,false,true,true,true);
    for(unsigned i=0;i<4;++i)sky_pipeline_[i]=pipeline("scene.vert",scene_shader,scene_pass_,true,(i&1)!=0,true,false,(i&2)!=0,false);
    for(unsigned i=0;i<5;++i)particle_pipeline_[i]=pipeline("particle.vert","particle.frag",scene_pass_,true,true,true,false,true,false,ParticleBlend(i));
    for(unsigned i=0;i<5;++i)after_particle_pipeline_[i]=pipeline("particle.vert","particle.frag",after_pass_,true,true,true,false,true,false,ParticleBlend(i));
    // Hilite state (0046ccf0 tail): CULLMODE CW = back-face shell, SRCALPHA/INVSRCALPHA, ZWRITE off, Z test on.
    contour_pipeline_=pipeline("contour.vert","contour.frag",scene_pass_,true,true,true,false,false,false,{},true);
     post_pipeline_=pipeline("fullscreen.vert","post.frag",post_pass_,false,false,false,false);
     overlay_pipeline_=pipeline("fullscreen.vert","overlay.frag",post_pass_,false,true,false,false);
     if(hud_pass_)hud_pipeline_=pipeline("fullscreen.vert","overlay.frag",hud_pass_,false,true,false,false);
     if(denoise_enabled_)temporal_pipeline_=pipeline("fullscreen.vert","temporal.frag",temporal_pass_,false,false,false,false);
     if(glow_pass_) {
         glow_pipeline_=pipeline("fullscreen.vert","glow.frag",glow_pass_,false,false,false,false);
         glow_composite_pipeline_=pipeline("fullscreen.vert","glow.frag",after_pass_,false,true,false,false,false,true,{},false,true);
     }
     immediate([&](VkCommandBuffer c) {
        for(auto* i:{&hdr_[0],&hdr_[1],&raw_hdr_,&guide_[0],&guide_[1],&glow_chain_[0],&glow_chain_[1],&fsr_output_,
                     &rt_diffuse_,&rt_reflection_,&diffuse_history_[0],&diffuse_history_[1],&reflection_history_[0],&reflection_history_[1],
                     &atrous_[0],&atrous_[1],&atrous_[2],&atrous_[3]}) {
             if(!i->handle)continue;
             barrier(c,i->handle,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
             VkClearColorValue clear{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
             vkCmdClearColorImage(c,i->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
             barrier(c,i->handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
         }
     });
    dirty_=false;rendered_=false;frame_=0;clear_material_sets();if(scene_)descriptors(); // material sets bind shadow_/depth_
    for(std::size_t i=0;i<particle_sets_.size();++i)write_set(particle_sets_[i],nullptr,0,false,false,&particle_images_[i]);
    return true;
 }
 const Image& texture(const std::string& name,const Image& fallback)const{if(name.empty())return fallback;auto found=texture_lookup_.find(name);if(found==texture_lookup_.end())throw std::runtime_error("scene references missing decoded texture: "+name);return textures_[found->second];}
VkDescriptorSet allocate_set(VkDescriptorSetLayout layout,VkDescriptorPool pool=VK_NULL_HANDLE){VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;a.descriptorPool=pool?pool:descriptor_pool_;a.descriptorSetCount=1;a.pSetLayouts=&layout;VkDescriptorSet result;checked(vkAllocateDescriptorSets(device_,&a,&result),"allocate descriptor set");return result;}
// temporal: input_diffuse/input_reflection replace the previous history at 6/8 (a-trous iterations read the last pass).
void write_set(VkDescriptorSet set,const Material* mat,unsigned history,bool post,bool temporal=false,const Image* diffuse=nullptr,const Image* second=nullptr,
               const Image* input_diffuse=nullptr,const Image* input_reflection=nullptr) {
     VkDescriptorBufferInfo buffer_info{uniform_.handle,0,sizeof(Frame)};
     std::array<VkDescriptorImageInfo,11> images{};
     const Image* maps[11]{&white_,&flat_normal_,&white_,&white_,&shadow_,&white_,&depth_,&overlay_,&white_,&white_,&white_};
     if(post)maps[0]=&hdr_[history];
     else if(mat) {
         const auto bound=material_maps(*mat);
         maps[0]=bound[0];maps[1]=bound[1];maps[2]=bound[2];maps[3]=bound[3];maps[8]=bound[4];
     }
    if(diffuse)maps[0]=diffuse;
    if(second)maps[1]=second;
    // temporal.frag bindings: 1 raw, 2/3 current diffuse/reflection, 4 motion, 6/8 previous diffuse/reflection history,
    // 10/11 current/previous guide.
    if(temporal){maps[0]=&raw_hdr_;maps[1]=&rt_diffuse_;maps[2]=&rt_reflection_;maps[3]=&motion_;maps[5]=&diffuse_history_[1-history];
        maps[7]=&reflection_history_[1-history];maps[9]=&guide_[history];maps[10]=&guide_[1-history];
        if(input_diffuse){maps[5]=input_diffuse;maps[7]=input_reflection;}}
     const unsigned image_count=denoise_enabled_?11:9;
     std::array<VkWriteDescriptorSet,12> writes{};
     writes[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[0].dstSet=set;writes[0].dstBinding=0;
     writes[0].descriptorCount=1;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
     writes[0].pBufferInfo=&buffer_info;
     for(unsigned i=0;i<image_count;++i) {
         images[i]={i==4||i==6||i>=9?shadow_sampler_:i==5||i==7||(post&&i<2)?linear_clamp_:sampler_,
             maps[i]->view,i==4||i==6?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         writes[i+1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i+1].dstSet=set;writes[i+1].dstBinding=i+1;
         writes[i+1].descriptorCount=1;writes[i+1].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
         writes[i+1].pImageInfo=&images[i];
     }
     vkUpdateDescriptorSets(device_,image_count+1,writes.data(),0,nullptr);
 }
std::array<const Image*,5> material_maps(const Material& m)const {
     return {&texture(m.diffuse,white_),&texture(m.normal,flat_normal_),
         &texture(!m.environment.empty()?m.environment:!m.luminance.empty()?m.luminance:m.lightmap,white_),
         &texture(m.luminance,white_),&texture(m.lightmap,white_)};
}
// A material set depends only on its five bound views (the rest are frame targets), so retained-scene reloads (actor AOI
// churn) reuse cached sets instead of rewriting thousands. Cleared whenever textures or render targets are destroyed.
VkDescriptorSet material_set(const Material& m) {
     const auto bound=material_maps(m);
     const std::array<VkImageView,5> key{bound[0]->view,bound[1]->view,bound[2]->view,bound[3]->view,bound[4]->view};
     const auto [found,inserted]=material_cache_.try_emplace(key,VK_NULL_HANDLE);
     if(!inserted)return found->second;
     try {
         VkResult result=VK_ERROR_OUT_OF_POOL_MEMORY;
         if(!material_pools_.empty()) {
             VkDescriptorSetAllocateInfo a{};a.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
             a.descriptorPool=material_pools_.back();a.descriptorSetCount=1;a.pSetLayouts=&set_layout_;
             result=vkAllocateDescriptorSets(device_,&a,&found->second);
         }
         if(result==VK_ERROR_OUT_OF_POOL_MEMORY||result==VK_ERROR_FRAGMENTED_POOL) {
             constexpr std::uint32_t chunk=1024;
             const VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,chunk},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,chunk*(denoise_enabled_?11u:9u)}};
             VkDescriptorPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;p.maxSets=chunk;p.poolSizeCount=2;p.pPoolSizes=sizes;
             material_pools_.push_back(VK_NULL_HANDLE);
             checked(vkCreateDescriptorPool(device_,&p,nullptr,&material_pools_.back()),"material descriptor pool");
             found->second=allocate_set(set_layout_,material_pools_.back());
         } else checked(result,"allocate material descriptor set");
         write_set(found->second,&m,0,false);
     } catch(...) {material_cache_.erase(found);throw;}
     return found->second;
}
void clear_material_sets() {
     for(auto pool:material_pools_)if(pool)vkDestroyDescriptorPool(device_,pool,nullptr);
     material_pools_.clear();material_cache_.clear();material_sets_.clear();
}
 void write_rt(){if(!rt_enabled_)return;VkAccelerationStructureKHR as=rt_->acceleration_structure();if(!as)throw std::runtime_error("RT scene has no acceleration structure");VkWriteDescriptorSetAccelerationStructureKHR acceleration{};acceleration.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;acceleration.accelerationStructureCount=1;acceleration.pAccelerationStructures=&as;VkDescriptorBufferInfo triangles{rt_->triangles(),0,rt_->triangle_bytes()};VkWriteDescriptorSet writes[2]{};writes[0] = {};writes[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[0].pNext=&acceleration;writes[0].dstSet=rt_set_;writes[0].dstBinding=0;writes[0].descriptorCount=1;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;writes[1] = {};writes[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[1].dstSet=rt_set_;writes[1].dstBinding=1;writes[1].descriptorCount=1;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[1].pBufferInfo=&triangles;vkUpdateDescriptorSets(device_,2,writes,0,nullptr);}
 // retained: actor attach/detach keeps every frame-target set (in-flight frames bind them) and only adds material sets.
 void descriptors(bool retained=false) {
     progress();
     material_usage_=scene_material_usage(*scene_);
     progress();
     if(!retained) {
         if(descriptor_pool_)vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
         descriptor_pool_=VK_NULL_HANDLE;
     }
     const std::uint32_t materials=static_cast<std::uint32_t>(std::max<std::size_t>(1,scene_->materials.size()));
     material_sets_.assign(materials,VK_NULL_HANDLE); // dormant materials are never bound (draw_scene rejects them)
     const Material none;
     for(std::uint32_t i=0;i<materials;++i) {
         if(i%1024==0)progress();
         if(!scene_->materials.empty()&&!material_usage_[i])continue;
         material_sets_[i]=material_set(i<scene_->materials.size()?scene_->materials[i]:none);
     }
     if(retained)return;
     const std::uint32_t sets=2+(rt_enabled_?1:0)+(denoise_enabled_?8:0)+(glow_pass_?4:0);
     VkDescriptorPoolSize sizes[4]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,sets},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,sets*(denoise_enabled_?11u:9u)},{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
     VkDescriptorPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
     p.maxSets=sets;p.poolSizeCount=rt_enabled_?4:2;p.pPoolSizes=sizes;
     checked(vkCreateDescriptorPool(device_,&p,nullptr,&descriptor_pool_),"descriptor pool");
     for(unsigned i=0;i<2;++i) {
         // With FSR the post pass tonemaps the upscaled output instead of the render-resolution history.
         post_sets_[i]=allocate_set(set_layout_);write_set(post_sets_[i],nullptr,i,true,false,fsr_?&fsr_output_:nullptr);
         temporal_sets_[i]=VK_NULL_HANDLE;
         if(denoise_enabled_) {
             temporal_sets_[i]=allocate_set(set_layout_);write_set(temporal_sets_[i],nullptr,i,true,true);
             const std::pair<const Image*,const Image*> inputs[3]{{&diffuse_history_[i],&reflection_history_[i]},{&atrous_[0],&atrous_[1]},{&atrous_[2],&atrous_[3]}};
             for(unsigned k=0;k<3;++k){atrous_sets_[i][k]=allocate_set(set_layout_);write_set(atrous_sets_[i][k],nullptr,i,true,true,nullptr,nullptr,inputs[k].first,inputs[k].second);}
         }
     }
     if(glow_pass_)for(unsigned i=0;i<4;++i) {
         glow_sets_[i]=allocate_set(set_layout_);
         write_set(glow_sets_[i],nullptr,i<2?i:0,true,false,i<2?nullptr:&glow_chain_[i==2?1:0],i<2?&glow_chain_[0]:nullptr);
     }
     if(rt_enabled_){rt_set_=allocate_set(rt_layout_);write_rt();}
 }
 std::uint64_t scene_hash(const Scene& s,std::span<const RenderEntity> entities,std::uint64_t* color_hash=nullptr)const {
     std::uint64_t h=1469598103934665603ull,color=1469598103934665603ull;
     auto source_draw=[&](std::uint32_t mesh,std::uint32_t override_material=UINT32_MAX,bool proxy=false,bool ordinary_participates=true) {
         if(proxy)return false;
         const bool visible=scene_source_color_draw(s,mesh,override_material,ordinary_participates);
         const auto material=override_material==UINT32_MAX?s.meshes[mesh].material:override_material;
         color=(color^mesh)*1099511628211ull;
         color=(color^material)*1099511628211ull;
         color=(color^visible)*1099511628211ull;
         return visible;
     };
     color=(color^s.instances.size())*1099511628211ull;
     if(s.instances.empty()) {
         for(std::uint32_t mesh=0;mesh<s.meshes.size();++mesh)source_draw(mesh);
     } else for(const auto& instance:s.instances) {
         if(!source_draw(instance.mesh,instance.material,instance.occlusion_proxy,instance.source_ordinary_color_participates))continue;
         h=(h^instance.mesh)*1099511628211ull;
         for(float v:instance.transform.matrix)h=(h^std::bit_cast<std::uint32_t>(v))*1099511628211ull;
         if(instance.entity_id) {
             const auto entity=std::find_if(entities.begin(),entities.end(),[&](const auto& value){return value.id==instance.entity_id;});
             h=(h^(entity!=entities.end()))*1099511628211ull;
             if(entity!=entities.end()) {
                 for(float value:{entity->x,entity->ground_y,entity->z,entity->yaw})
                     h=(h^std::bit_cast<std::uint32_t>(value))*1099511628211ull;
             }
         }
     }
     if(color_hash)*color_hash=color;
     h=(h^color)*1099511628211ull;
     for(const auto* values:{&s.lighting.sun_direction,&s.lighting.ambient_multiplier,
                             &s.lighting.diffuse_multiplier,&s.lighting.fog_color})
         for(float v:*values)h=(h^std::bit_cast<std::uint32_t>(v))*1099511628211ull;
     h=(h^std::bit_cast<std::uint32_t>(s.lighting.fog_start))*1099511628211ull;
     h=(h^std::bit_cast<std::uint32_t>(s.lighting.fog_end))*1099511628211ull;
     return h;
 }
 // A new Scene waits for the device and reloads everything. An actor attach/detach (same Scene) keeps every other mesh's
 // buffers (moved by key) and never waits: new uploads are recorded into this frame (command_, already begun), released
 // buffers retire until the frames using them completed, frame-target descriptor sets stay, and render() updates RT
 // in-frame (rt_stale_).
 void load(const Scene& s,std::span<const RenderEntity> entities) {
     const auto started=std::chrono::steady_clock::now();
     const bool retain=scene_==&s&&s.textures.size()>=textures_.size();
     if(!retain)wait_idle("scene upload wait");
     if(!retain) {
         if(descriptor_pool_)vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
         descriptor_pool_=VK_NULL_HANDLE;post_sets_[0]=post_sets_[1]=rt_set_=VK_NULL_HANDLE;
         clear_material_sets();
         for(auto& g:geometry_){progress();destroy(g);}
         geometry_.clear();
         for(auto& t:textures_){progress();destroy(t);}
         textures_.clear();texture_lookup_.clear();
     }
     scene_=nullptr;
     std::size_t released_meshes=0;
     if(retain) {
         const auto remap=scene_mesh_remap(mesh_keys_,s);
         std::vector<Geometry> kept(s.meshes.size());
         for(std::size_t i=0;i<remap.size();++i)if(remap[i]!=SIZE_MAX)kept[i]=std::exchange(geometry_[remap[i]],Geometry{});
         for(auto& g:geometry_)if(g.vertices.handle||g.indices.handle||g.staging[0].handle) {
             retire(g.vertices);retire(g.indices);for(auto& b:g.staging)retire(b);retire(g.previous);++released_meshes;
         }
         geometry_=std::move(kept);
     }
     const auto retained_meshes=std::count_if(geometry_.begin(),geometry_.end(),
         [](const Geometry& g){return g.vertices.handle!=VK_NULL_HANDLE;});
     const auto retained_textures=textures_.size();
     std::size_t new_meshes=0,submissions=0,memory_blocks=0,texture_bytes=0;
     VkDeviceSize upload_bytes=0;
     try {
         std::vector<std::uint8_t> used(s.meshes.size(),0);
         if(s.instances.empty()) {
             for(std::uint32_t mesh=0;mesh<s.meshes.size();++mesh) {
                 if(mesh%4096==0)progress();
                 used[mesh]=scene_source_color_draw(s,mesh)?2:1;
             }
         } else for(const auto& instance:s.instances) {
             if((&instance-s.instances.data())%1024==0)progress(); // per-item network pumps cost ~4 ms per actor-churn reload
             if(instance.mesh>=s.meshes.size())throw std::runtime_error("original instance mesh index out of range");
             if(instance.occlusion_proxy)continue;
             const std::uint8_t residency=scene_source_color_draw(s,instance.mesh,instance.material,instance.source_ordinary_color_participates)?2:1;
             used[instance.mesh]=std::max(used[instance.mesh],residency);
         }
         geometry_.resize(s.meshes.size());
         textures_.resize(s.textures.size());
         MemoryPool pool; // full loads only: retained reloads (actor churn) allocate and retire per resource
         UploadBatch batch(*this,retain?command_:VK_NULL_HANDLE,retain?nullptr:&pool);
         for(std::size_t mesh_index=0;mesh_index<s.meshes.size();++mesh_index) {
             if(mesh_index%1024==0)progress(); // uploads pump on their own
             if(!used[mesh_index])continue;
             auto& g=geometry_[mesh_index];
             if(g.vertices.handle)continue; // Retained: validated when uploaded; skins update through update_geometry.
             const auto& mesh=s.meshes[mesh_index];
             if(mesh.vertices.empty()||mesh.indices.empty()||mesh.indices.size()%3)
                 throw std::runtime_error("invalid original indexed mesh");
             if(mesh.vertices.size()>assets::max_entry_bytes/sizeof(Vertex)||
                mesh.indices.size()>assets::max_entry_bytes/sizeof(std::uint32_t)||
                mesh.indices.size()>UINT32_MAX)
                 throw std::runtime_error("original indexed mesh exceeds upload limit");
             for(std::size_t index=0;index<mesh.indices.size();++index) {
                 if(index%4096==0)progress();
                 if(mesh.indices[index]>=mesh.vertices.size())
                     throw std::runtime_error("original mesh index out of range");
             }
             if(used[mesh_index]!=2)continue;
             // Indexed slots own destinations even if a later upload fails; batch staging outlives submitted work.
             const bool skinned=!mesh.influences.empty();
             g.vertices=upload_buffer(mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|(skinned?VkBufferUsageFlags{VK_BUFFER_USAGE_TRANSFER_SRC_BIT}:0u),batch);
             g.indices=upload_buffer(mesh.indices.data(),mesh.indices.size()*sizeof(std::uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,batch);
             if(skinned) {
                 for(auto& stage:g.staging)
                     stage=buffer(g.vertices.bytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,batch.pool);
                 // Motion vectors' previous pose starts equal to the bind pose; record_geometry_uploads swaps it with
                 // vertices, so both carry the same usage.
                 g.previous=upload_buffer(mesh.vertices.data(),g.vertices.bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,batch);
             }
             g.revision=mesh.revision;g.count=static_cast<std::uint32_t>(mesh.indices.size());g.bounds=mesh_bounds(mesh);++new_meshes;
         }
         for(std::size_t i=retained_textures;i<s.textures.size();++i) {
             progress();
             const auto& t=s.textures[i];
             if(!texture_lookup_.emplace(t.name,i).second)throw std::runtime_error("duplicate original texture name");
             textures_[i]=upload_texture(t,&batch);texture_bytes+=t.data.size();
         }
         batch.flush();submissions=batch.submissions;upload_bytes=batch.total_bytes;memory_blocks=pool.allocations;
         // Appending must not conceal pending revisions of already-retained animated meshes.
         update_geometry(s);geometry_revision_=s.geometry_revision;
         mesh_keys_.resize(s.meshes.size());for(std::size_t i=0;i<s.meshes.size();++i)mesh_keys_[i]=s.meshes[i].key;mesh_key_=s.mesh_key;
         if(rt_enabled_&&!retain){std::scoped_lock lock(queue_mutex_);rt_->rebuild(s,entities);} // RT builds submit to queue_
         rt_stale_=rt_enabled_&&retain;
         history_instance_models_.resize(s.instances.size());
         scene_=&s;transform_hash_=scene_hash(s,entities,&color_resource_hash_);instance_count_=s.instances.size();frame_=0;fsr_reset_=fsr_reset_||!retain;descriptors(retain);
     } catch(...) {scene_=nullptr;throw;}
     const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
     std::cerr<<"Vulkan scene resources mode="<<(retain?"retain":"full")
         <<" meshesNew="<<new_meshes<<" meshesRetained="<<retained_meshes<<" meshesReleased="<<released_meshes
         <<" texturesNew="<<s.textures.size()-retained_textures<<" texturesRetained="<<retained_textures
         <<" uploadBatches="<<submissions<<" uploadBytes="<<upload_bytes<<" textureBytes="<<texture_bytes
         <<" textureBC="<<(bc_supported_?1:0)<<" memoryBlocks="<<memory_blocks<<" elapsedMs="<<elapsed<<'\n';
 }
// Stages changed skins in the current slot; record_geometry_uploads copies them in this frame's command buffer.
void update_geometry(const Scene& scene) {
    if(scene.meshes.size()!=geometry_.size())throw std::runtime_error("scene topology changed without scene reload");
    for(std::size_t i=0;i<scene.meshes.size();++i) {
        if(i%4096==0)progress();
        if(!geometry_[i].vertices.handle||scene.meshes[i].influences.empty()||geometry_[i].revision==scene.meshes[i].revision)continue;
        auto& g=geometry_[i];const auto& vertices=scene.meshes[i].vertices;auto& stage=g.staging[total_frames_&1u];
        if(vertices.size()*sizeof(Vertex)!=g.vertices.bytes||!stage.mapped)
            throw std::runtime_error("skinned topology changed without scene reload");
        copy_staging(stage.mapped,vertices.data(),static_cast<std::size_t>(g.vertices.bytes));
        g.upload=true;
    }
    for(std::size_t i=0;i<scene.meshes.size();++i)geometry_[i].revision=scene.meshes[i].revision;
}
void record_geometry_uploads() {
    bool any=false;
    const auto begin=[&] { // earlier frames' vertex fetches and copies of the shared buffers finish before they're reused
        if(any)return;
        VkMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,1,&b,0,nullptr,0,nullptr);
        any=true;
    };
    // Motion vectors' previous pose: an uploaded skin swaps vertices/previous (the buffer the last frame drew becomes
    // previous) and writes the new pose into the other one, instead of copying vertices->previous first. A skin that
    // stops changing copies once, so previous == current from the next frame on.
    for(std::size_t i=0;i<geometry_.size();++i) {
        if(i%4096==0)progress();
        auto& g=geometry_[i];
        if(g.upload) {
            begin();
            if(motion_vectors_&&g.previous.handle)std::swap(g.vertices,g.previous);
            VkBufferCopy copy{0,0,g.vertices.bytes};
            vkCmdCopyBuffer(command_,g.staging[total_frames_&1u].handle,g.vertices.handle,1,&copy);
            g.upload=false;g.previous_stale=g.previous.handle!=VK_NULL_HANDLE;
        } else if(g.previous_stale&&motion_vectors_) {
            begin();
            VkBufferCopy copy{0,0,g.vertices.bytes};
            vkCmdCopyBuffer(command_,g.vertices.handle,g.previous.handle,1,&copy);
            g.previous_stale=false;
        }
    }
    if(!any)return;
    VkMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,0,1,&b,0,nullptr,0,nullptr);
}
 Matrix4 light_matrix()const {
     auto sun=scene_&&scene_->lighting.source_values?scene_->lighting.sun_direction:std::array<float,3>{.48f,.78f,.4f};
     Camera light=options_.camera;
     light.yaw=std::atan2(sun[0],sun[2]);light.pitch=std::atan2(sun[1],std::sqrt(sun[0]*sun[0]+sun[2]*sun[2]));
     light.pitch=std::clamp(light.pitch,-1.54f,1.54f);
     light.distance=std::max(50.f,options_.camera.distance*3);light.near_plane=.1f;light.far_plane=light.distance*4;
     const auto basis=camera_basis(light);
     auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
     Matrix4 view{basis.right[0],basis.up[0],-basis.forward[0],0,basis.right[1],basis.up[1],-basis.forward[1],0,
         basis.right[2],basis.up[2],-basis.forward[2],0,-dot(basis.right,basis.eye),-dot(basis.up,basis.eye),dot(basis.forward,basis.eye),1};
     float radius=std::max(30.f,options_.camera.distance*2);
     // Whole shadow-map texels only: a window following the player by fractions of a texel re-rasterised every leaf
     // shadow at a new sub-texel phase each frame (crawling shadows while walking).
     const float texel=2*radius/2048;
     for(std::size_t i:{12u,13u})view[i]=std::round(view[i]/texel)*texel;
     Matrix4 projection{};projection[0]=1/radius;projection[5]=-1/radius;
     projection[10]=1/(light.near_plane-light.far_plane);
     projection[14]=light.near_plane/(light.near_plane-light.far_plane);projection[15]=1;
     return multiply_matrix(projection,view);
 }
 void draw_scene(VkCommandBuffer c,bool shadow) {
     if(rt_enabled_&&!shadow)vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout_,1,1,&rt_set_,0,nullptr);
     // draws_ is sorted by pass, then distance: skip rebinding an unchanged pipeline, material set and mesh buffers.
     VkPipeline bound_pipeline=VK_NULL_HANDLE;VkDescriptorSet bound_set=VK_NULL_HANDLE;std::uint32_t bound_mesh=UINT32_MAX;
     for(const auto& item:draws_) {
         if(shadow?item.sky:!item.visible)continue;
         if(!scene_->materials.empty()) {
             if(item.material>=scene_->materials.size()||item.material>=material_usage_.size())
                 throw std::runtime_error("scene draw material index out of range");
             if(!material_usage_[item.material])
                 throw std::runtime_error("scene draw references an unbound dormant material: "+std::to_string(item.material));
         }
         const Material* material=item.material<scene_->materials.size()?&scene_->materials[item.material]:nullptr;
         Draw draw;draw.model=item.model;
         draw.color=material?material->color:std::array<float,4>{1,1,1,1};
         draw.flags={material&&!material->normal.empty()?1.f:0.f,material&&(!material->environment.empty()||(item.sky&&(!material->luminance.empty()||!material->lightmap.empty())))?1.f:0.f,
             material&&!material->luminance.empty()?1.f:0.f,material?material->alpha_cutoff:0.f};
        // extra.w packs alpha blending (1), source ocean (2) and animated draws (.5; guided history needs motion vectors).
        draw.extra={material&&!material->lightmap.empty()?1.f:0.f,item.sky?1.f:0.f,item.fog?1.f:0.f,
            (item.alpha?1.f:0.f)+(item.dynamic?.5f:0.f)+(material&&material->source_ocean?2.f:0.f)};
         draw.scroll=item.scroll;draw.previous_model=item.previous;
         bool double_sided=material&&material->double_sided;
         auto selected=shadow?(double_sided?shadow_double_:shadow_pipeline_):
             item.sky?sky_pipeline_[(double_sided?2:0)+(item.alpha?1:0)]:
             item.alpha?(double_sided?alpha_double_:alpha_):(double_sided?opaque_double_:opaque_);
         if(selected!=bound_pipeline){vkCmdBindPipeline(c,VK_PIPELINE_BIND_POINT_GRAPHICS,selected);bound_pipeline=selected;}
         const auto set=material_sets_[item.material<material_sets_.size()?item.material:0];
         if(set!=bound_set){vkCmdBindDescriptorSets(c,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout_,0,1,&set,1,&uniform_offset_);bound_set=set;}
         vkCmdPushConstants(c,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
         auto& g=geometry_[item.mesh];
         if(item.mesh!=bound_mesh) {
             // Binding 1 = the previous pose motion vectors compare against (skins only, once motion is rendered).
             const VkBuffer buffers[2]{g.vertices.handle,motion_vectors_&&g.previous.handle?g.previous.handle:g.vertices.handle};
             const VkDeviceSize offsets[2]{};
             vkCmdBindVertexBuffers(c,0,2,buffers,offsets);
             vkCmdBindIndexBuffer(c,g.indices.handle,0,VK_INDEX_TYPE_UINT32);
             bound_mesh=item.mesh;
         }
         vkCmdDrawIndexed(c,g.count,1,0,0,0);
     }
 }
 // depth: clear the depth attachment that follows the pass's colour attachments (location 0 clears to the backdrop,
 // further colours/histories/motion/reactive to zero).
 void begin_pass(VkCommandBuffer c,VkRenderPass pass,VkFramebuffer fb,VkExtent2D size,bool depth) {
     const std::uint32_t colors=pass==shadow_pass_?0:attachments(colors_of(pass));
     std::array<VkClearValue,9> clears{};
     if(colors)clears[0].color={{.04f,.05f,.065f,1}};
     if(depth)clears[colors].depthStencil={1,0};
     VkRenderPassBeginInfo b{};b.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
     b.renderPass=pass;b.framebuffer=fb;b.renderArea.extent=size;
     b.clearValueCount=colors+(depth?1:0);b.pClearValues=clears.data();
     vkCmdBeginRenderPass(c,&b,VK_SUBPASS_CONTENTS_INLINE);
     VkViewport viewport{0,0,float(size.width),float(size.height),0,1};VkRect2D scissor{{0,0},size};
     vkCmdSetViewport(c,0,1,&viewport);vkCmdSetScissor(c,0,1,&scissor);
 }
 void cleanup()noexcept {
     if(presenter_.joinable()) { // finishes queued presents first
         {std::scoped_lock lock(present_mutex_);present_stop_=true;}
         present_cv_.notify_all();presenter_.join();
     }
     if(device_) {
         for(auto& slot:slots_)if(slot.pending){drain_fence(slot.fence);slot.pending=false;}
         VkResult idle;
         do {idle=vkDeviceWaitIdle(device_);}while(idle!=VK_SUCCESS&&idle!=VK_ERROR_DEVICE_LOST);
         try {
             if(cache_&&!cache_path_.empty()) {
                 std::size_t bytes=0;
                 if(vkGetPipelineCacheData(device_,cache_,&bytes,nullptr)==VK_SUCCESS&&bytes<32*1024*1024) {
                     std::vector<char> data(bytes);
                     if(vkGetPipelineCacheData(device_,cache_,&bytes,data.data())==VK_SUCCESS) {
                         std::ofstream out(cache_path_,std::ios::binary);
                         out.write(data.data(),static_cast<std::streamsize>(bytes));
                     }
                 }
             }
         } catch(...) {} // Cache persistence is optional; GPU lifetime cleanup is not.
         destroy_targets();
         if(descriptor_pool_)vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
         clear_material_sets();
         rt_.reset();
         for(auto& g:geometry_)destroy(g);
         for(auto& entry:retired_)destroy(entry.second);
         for(auto& t:textures_)destroy(t);
        destroy(overlay_);for(auto& b:overlay_staging_)destroy(b);destroy(white_);destroy(flat_normal_);destroy(uniform_);
        for(auto& image:particle_images_)destroy(image);
        for(auto& b:particle_buffer_)destroy(b);
        if(particle_pool_)vkDestroyDescriptorPool(device_,particle_pool_,nullptr);
         if(cache_)vkDestroyPipelineCache(device_,cache_,nullptr);
         if(pipeline_layout_)vkDestroyPipelineLayout(device_,pipeline_layout_,nullptr);
         if(set_layout_)vkDestroyDescriptorSetLayout(device_,set_layout_,nullptr);
         if(rt_layout_)vkDestroyDescriptorSetLayout(device_,rt_layout_,nullptr);
         if(sampler_)vkDestroySampler(device_,sampler_,nullptr);
         if(shadow_sampler_)vkDestroySampler(device_,shadow_sampler_,nullptr);
         if(linear_clamp_)vkDestroySampler(device_,linear_clamp_,nullptr);
         for(auto& slot:slots_) {
             if(slot.fence)vkDestroyFence(device_,slot.fence,nullptr);
             if(slot.acquire)vkDestroySemaphore(device_,slot.acquire,nullptr);
         }
         for(unsigned i=0;i<2;++i) {
             if(present_fences_[i])vkDestroyFence(device_,present_fences_[i],nullptr);
             if(present_acquire_[i])vkDestroySemaphore(device_,present_acquire_[i],nullptr);
         }
         if(present_pool_)vkDestroyCommandPool(device_,present_pool_,nullptr);
         if(pool_)vkDestroyCommandPool(device_,pool_,nullptr);
         vkDestroyDevice(device_,nullptr);
     }
     if(surface_)SDL_Vulkan_DestroySurface(instance_,surface_,nullptr);
     if(instance_)vkDestroyInstance(instance_,nullptr);
 }
public:
 VulkanRenderer(SDL_Window* window,const RendererOptions& options):window_(window),options_(options){if(!window_)throw std::invalid_argument("null Vulkan window");try{init();}catch(...){cleanup();throw;}}
 ~VulkanRenderer()override{cleanup();}
 const Camera& camera()const override{return options_.camera;}
 void set_camera(const Camera& camera)override{validate_camera(camera);options_.camera=camera;}
 void resize(int,int)override{dirty_=true;}
 void set_overlay(const Overlay& o,std::span<const std::array<int,4>> changed)override {
     if(o.width<0||o.height<0||o.rgba.size()!=std::uint64_t(o.width)*std::uint64_t(o.height)*4||
        std::uint64_t(o.width)*std::uint64_t(o.height)>268435456)
         throw std::invalid_argument("invalid overlay RGBA size");
     overlay_visible_=o.width>0&&o.height>0;
     if(!overlay_visible_)return;
     const auto width=static_cast<std::uint32_t>(o.width),height=static_cast<std::uint32_t>(o.height);
     bool whole=changed.empty()||overlay_undefined_;
     if(overlay_.width!=width||overlay_.height!=height) {
         whole=true;
         wait_frame("overlay frame wait"); // in-flight frames sample the old image through these descriptors
         auto next=image(width,height,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
         destroy(overlay_);overlay_=next;overlay_undefined_=true;
         update_overlay_descriptors();
     }
     wait_slot(total_frames_&1u,"overlay staging wait"); // the slot's previous frame copied from this staging buffer
     auto& stage=overlay_staging_[total_frames_&1u];
     if(stage.bytes<VkDeviceSize(width)*height*4) {
         destroy(stage);
         stage=buffer(VkDeviceSize(width)*height*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
     }
     if(whole){
         copy_staging(stage.mapped,o.rgba.data(),o.rgba.size());
         overlay_regions_.clear();overlay_whole_=true;
     }else for(const auto& r:changed) {
         // Only the changed rectangles: the image keeps every other texel from earlier uploads.
         const auto x0=std::clamp(r[0],0,o.width),y0=std::clamp(r[1],0,o.height);
         const auto x1=std::clamp(r[0]+std::max(r[2],0),x0,o.width),y1=std::clamp(r[1]+std::max(r[3],0),y0,o.height);
         if(x0==x1||y0==y1)continue;
         const auto offset=(std::size_t(y0)*width+std::size_t(x0))*4,row=std::size_t(x1-x0)*4;
         for(auto y=y0;y<y1;++y)std::memcpy(static_cast<std::byte*>(stage.mapped)+offset+std::size_t(y-y0)*width*4,o.rgba.data()+offset+std::size_t(y-y0)*width*4,row);
         if(overlay_whole_)continue; // a whole upload already pending this frame copies the updated staging
         VkBufferImageCopy copy{};copy.bufferOffset=offset;copy.bufferRowLength=width;copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
         copy.imageOffset={x0,y0,0};copy.imageExtent={std::uint32_t(x1-x0),std::uint32_t(y1-y0),1};overlay_regions_.push_back(copy);
     }
     overlay_upload_=overlay_whole_||!overlay_regions_.empty();
 }
 void record_overlay_upload(VkCommandBuffer c) {
     const auto& stage=overlay_staging_[total_frames_&1u];
     barrier(c,overlay_.handle,overlay_undefined_?VK_IMAGE_LAYOUT_UNDEFINED:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,overlay_undefined_?0:VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,
         overlay_undefined_?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
     VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={overlay_.width,overlay_.height,1};
     if(overlay_whole_)vkCmdCopyBufferToImage(c,stage.handle,overlay_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
     else vkCmdCopyBufferToImage(c,stage.handle,overlay_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,std::uint32_t(overlay_regions_.size()),overlay_regions_.data());
     barrier(c,overlay_.handle,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
     overlay_upload_=overlay_undefined_=overlay_whole_=false;overlay_regions_.clear();
 }
 void update_overlay_descriptors() {
     if(!descriptor_pool_)return;
     {
         VkDescriptorImageInfo info{linear_clamp_,overlay_.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
         auto update=[&](VkDescriptorSet set) {
             VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
             write.dstSet=set;write.dstBinding=8;write.descriptorCount=1;
             write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&info;
             vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
         };
         for(const auto& [views,set]:material_cache_)update(set);
        // Temporal sets bind reflection history at 8, not the overlay.
        for(unsigned i=0;i<2;++i)update(post_sets_[i]);
     }
 }
void set_grayscale(bool on)override { grayscale_=on; }
void set_upscaling(Upscaler upscaler,FsrQuality quality,float sharpness,bool frame_generation)override {
    if(!std::isfinite(sharpness)||sharpness<0||sharpness>1)throw std::invalid_argument("FSR sharpness requires [0,1]");
    // Render extent, targets, or the frame-generation contexts/swapchain owner change.
    if(upscaler!=options_.upscaler||quality!=options_.fsr_quality||frame_generation!=options_.frame_generation)dirty_=true;
    options_.upscaler=upscaler;options_.fsr_quality=quality;options_.fsr_sharpness=sharpness;options_.frame_generation=frame_generation;
}
void set_particles(const ParticleFrame& frame)override {
    for(const auto& batch:frame.batches)
        if(batch.texture>=frame.textures.size()||batch.first>frame.vertices.size()||batch.count>frame.vertices.size()-batch.first||batch.count%6||
           static_cast<unsigned>(batch.blend)>4||!std::isfinite(batch.depth_bias)||!std::isfinite(batch.alpha_reference))
            throw std::invalid_argument("invalid particle batch");
    if(frame.textures.size()<particle_images_.size()) {
        wait_frame("particle frame wait"); // the in-flight frame binds these sets
        for(auto& image:particle_images_)destroy(image);
        particle_images_.clear();particle_sets_.clear();
        if(particle_pool_)checked(vkResetDescriptorPool(device_,particle_pool_,0),"reset particle descriptors");
    }
    // ponytail: fixed 256-texture pool; the original XML references 64 particle textures plus effect icons.
    constexpr std::uint32_t capacity=256;
    if(!particle_pool_&&!frame.textures.empty()) {
        VkDescriptorPoolSize sizes[2]{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,capacity},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,capacity*11}};
        VkDescriptorPoolCreateInfo p{};p.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;p.maxSets=capacity;p.poolSizeCount=2;p.pPoolSizes=sizes;
        checked(vkCreateDescriptorPool(device_,&p,nullptr,&particle_pool_),"particle descriptor pool");
    }
    while(particle_images_.size()<frame.textures.size()) {
        if(particle_images_.size()>=capacity)throw std::runtime_error("particle texture descriptor capacity exceeded");
        const auto& t=frame.textures[particle_images_.size()];
        particle_images_.push_back(upload_texture(t));
        particle_sets_.push_back(allocate_set(set_layout_,particle_pool_));
        if(!dirty_)write_set(particle_sets_.back(),nullptr,0,false,false,&particle_images_.back()); // else create_targets writes it
    }
    const auto bytes=VkDeviceSize(frame.vertices.size())*sizeof(ParticleVertex);
    // Write the upcoming frame's slot after the last frame that drew it (latest batches persist across frames that set none).
    const unsigned slot=total_frames_&1u;
    wait_serial(particle_drawn_[slot],"particle frame wait");
    auto& vertices=particle_buffer_[slot];particle_slot_=slot;
    if(bytes>vertices.bytes) {
        destroy(vertices);
        vertices=buffer(std::max<VkDeviceSize>(bytes+bytes/2,65536),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if(bytes)std::memcpy(vertices.mapped,frame.vertices.data(),static_cast<std::size_t>(bytes));
    particle_batches_=frame.batches;
}
 void render(const Scene& scene,std::span<const RenderEntity> entities,std::uint32_t,float)override {
     const unsigned slot=total_frames_&1u;
     wait_slot(slot,"frame wait"); // frame total_frames_-2: its command buffer, uniforms and staging are reusable
     if(present_stale_.exchange(false))dirty_=true; // the present thread saw an out-of-date swapchain
     if(dirty_&&!create_targets())return;
     // Acquire before any scene/history state changes, so an out-of-date swapchain skips the frame without partial updates.
     // With frame generation the present thread owns the swapchain instead.
     std::uint32_t image_index=0;
     if(!generating_) {
         VkResult acquired;
         do {
             progress();
             acquired=vkAcquireNextImageKHR(device_,swapchain_,50'000'000,slots_[slot].acquire,VK_NULL_HANDLE,&image_index);
         } while(acquired==VK_TIMEOUT||acquired==VK_NOT_READY);
         if(acquired==VK_ERROR_OUT_OF_DATE_KHR){dirty_=true;return;}
         if(acquired!=VK_SUCCESS&&acquired!=VK_SUBOPTIMAL_KHR)checked(acquired,"acquire image");
     }
     // Begun before any reload: a retained (actor attach/detach) reload records its uploads into this frame.
     command_=slots_[slot].command;
     checked(vkResetCommandBuffer(command_,0),"reset frame command");
     VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
     begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
     checked(vkBeginCommandBuffer(command_,&begin),"begin frame");
     if(scene_!=&scene || mesh_key_!=scene.mesh_key || geometry_.size()!=scene.meshes.size() || textures_.size()!=scene.textures.size() || material_usage_.size()!=scene.materials.size() || instance_count_!=scene.instances.size()) {
         load(scene,entities);
     }
     std::uint64_t color_hash=0;
     auto hash=scene_hash(scene,entities,&color_hash);
     if(color_hash!=color_resource_hash_)load(scene,entities);
     bool geometry_changed=geometry_revision_!=scene.geometry_revision;
     if(geometry_changed) {
         update_geometry(scene);
         geometry_revision_=scene.geometry_revision;
     }
     const bool scene_changed=hash!=transform_hash_||geometry_changed;
     if(overlay_upload_)record_overlay_upload(command_);
     record_geometry_uploads();
     if(scene_changed||rt_stale_) {
         // Skin/transform changes and actor attach/detach update the BLAS/TLAS inside this frame, ordered after earlier
         // frames' ray queries. Anything else rebuilds and rebinds once every frame in flight completed (no frame sees a
         // retired AS).
         if(rt_enabled_&&!rt_->record_refresh(scene,entities,command_,slot)) {
             wait_frame("RT rebuild wait");
             {std::scoped_lock lock(queue_mutex_);rt_->rebuild(scene,entities);}
             write_rt();
         }
         transform_hash_=hash;rt_stale_=false;
     }
     const auto basis=camera_basis(options_.camera);
     auto eye=basis.eye;
     const auto source_ticks=static_cast<std::uint32_t>(SDL_GetTicks());
     draws_.clear();contours_.clear();
     // Frustum culling marks draws instead of dropping them, so the sort below orders exactly the unculled list. Sky,
     // source ocean (vertex-displaced) and skinned (per-frame world-space) meshes always draw; enhanced raster keeps every
     // draw for the shadow pass.
     const bool cull=options_.original_appearance||rt_enabled_;
     const auto planes=frustum_planes(camera_view_projection(options_.camera,static_cast<int>(extent_.width),static_cast<int>(extent_.height),true));
     // previous: last frame's model matrix (motion vectors); null = unmoved.
     auto add=[&](std::uint32_t mesh,const Matrix4& matrix,std::uint32_t override_material=UINT32_MAX,
                  bool sky=false,bool fog=false,bool dynamic=false,std::array<float,4> scroll={},bool visible=true,const Matrix4* previous=nullptr) {
         auto material=override_material==UINT32_MAX?scene.meshes[mesh].material:override_material;
         bool alpha=material<scene.materials.size()&&(scene.materials[material].alpha||scene.materials[material].color[3]<.99f);
         float dx=matrix[12]-eye[0],dy=matrix[13]-eye[1],dz=matrix[14]-eye[2];
         draws_.push_back({mesh,material,matrix,dx*dx+dy*dy+dz*dz,alpha,sky,fog,dynamic||!scene.meshes[mesh].influences.empty(),scroll,visible,previous?*previous:matrix});
     };
     if(scene.instances.empty()) {
         for(std::uint32_t i=0;i<scene.meshes.size();++i)
             if(scene_source_color_draw(scene,i))add(i,Transform{}.matrix);
     } else {
         for(const auto& instance:scene.instances) {
             if(instance.occlusion_proxy)continue;
             const auto scroll=(instance.ocean&&instance.source_ordinary_color_participates)?
                 advance_source_scroll(source_scroll_,static_cast<std::uint32_t>(SDL_GetTicks())):std::array<float,4>{};
             if(!scene_source_color_draw(scene,instance.mesh,instance.material,instance.source_ordinary_color_participates))continue;
             auto matrix=instance.transform.matrix;
             const RenderEntity* entity=nullptr;
             if(instance.entity_id) {
                 const auto p=std::find_if(entities.begin(),entities.end(),[&](const auto& value){return value.id==instance.entity_id;});
                 if(p==entities.end())continue;
                 entity=&*p;
                 matrix=entity_instance_transform(scene,instance,entity->x,entity->ground_y,entity->z,entity->yaw).matrix;
             }
             bool moved=false;Matrix4 previous_matrix=matrix;
             if(!instance.sky) {
                 auto& previous_model=history_instance_models_[std::size_t(&instance-scene.instances.data())];
                 if(frame_!=0)previous_matrix=previous_model;
                 moved=denoise_enabled_&&frame_!=0&&previous_model!=matrix;
                 previous_model=matrix;
             }
             std::array<float,3> center{};
             const bool test_fog=scene.lighting.fog_enabled&&!instance.sky;
             if(test_fog) {
                 if(instance.node>=scene.nodes.size())throw std::runtime_error("source fog instance node index out of range");
                 center=entity?
                     entity_source_bounds_center(scene,instance,entity->x,entity->ground_y,entity->z,entity->yaw):
                     scene.nodes[instance.node].source_bounds_center;
             }
             if(instance.sky) {
                 const float c=std::cos(scene.lighting.sky_rotation),s=std::sin(scene.lighting.sky_rotation);
                 matrix={c,0,-s,0,0,1,0,0,s,0,c,0,eye[0],eye[1],eye[2],1};
                 const auto& last=frame_!=0?previous_frame_.eye:std::array<float,4>{eye[0],eye[1],eye[2],1};
                 previous_matrix={c,0,-s,0,0,1,0,0,s,0,c,0,last[0],last[1],last[2],1}; // follows last frame's eye
             }
             const auto material=instance.material==UINT32_MAX?scene.meshes[instance.mesh].material:instance.material;
             const bool visible=!cull||instance.sky||instance.ocean||!scene.meshes[instance.mesh].influences.empty()||
                 (material<scene.materials.size()&&scene.materials[material].source_ocean)||
                 sphere_visible(planes,matrix,geometry_[instance.mesh].bounds);
             const bool fog=visible&&test_fog&&source_fog_enabled(instance.source_half_extents,center,matrix,eye,scene.lighting.fog_start);
             add(instance.mesh,matrix,instance.material,instance.sky,fog,instance.entity_id!=0||moved,scroll,visible,&previous_matrix);
             if(!visible)continue;
             if(entity&&!instance.sky&&entity->contour!=std::array<float,3>{}) {
                 for(float v:entity->contour)if(!std::isfinite(v))throw std::invalid_argument("non-finite entity contour colour");
                 contours_.push_back({instance.mesh,instance.material==UINT32_MAX?scene.meshes[instance.mesh].material:instance.material,matrix,entity->contour});
             }
         }
     }
     // In-place sorting avoids stable_sort's temporary allocation every frame.
     std::sort(draws_.begin(),draws_.end(),[](const auto& a,const auto& b) {
         if(a.sky!=b.sky)return a.sky;
         if(a.alpha!=b.alpha)return !a.alpha;
         // Opaque nearest-first: early Z rejects hidden fragments before their ray queries (-13% scene pass,
         // analysis/porting-wave12/Perf/report.md). Equal distances (meshes of one placement) fall back to material/mesh,
         // so coplanar opaque parts never swap draw order between frames.
         if(a.alpha)return a.distance>b.distance;
         if(a.distance!=b.distance)return a.distance<b.distance;
         return a.material!=b.material?a.material<b.material:a.mesh<b.mesh;
     });
     Frame f{};
     f.vp=camera_view_projection(options_.camera,static_cast<int>(extent_.width),static_cast<int>(extent_.height),true);
     std::array<float,2> fsr_jitter{};
     if(fsr_) { // FSR's Halton(2,3) sub-pixel offset: clip.xy += jitter*w moves the image +jitter render pixels (y down)
         fsr_jitter=fsr_->jitter(jitter_frame_++);
         const float jx=2*fsr_jitter[0]/float(render_extent_.width),jy=2*fsr_jitter[1]/float(render_extent_.height);
         for(std::size_t column=0;column<4;++column){f.vp[column*4]+=jx*f.vp[column*4+3];f.vp[column*4+1]+=jy*f.vp[column*4+3];}
         f.jitter={jx,jy,0,0};
         // No negative texture mip bias (AMD suggests log2(render/display)-1): measured, it raised FSR leaf and ground
         // flicker in motion (analysis/porting-wave11/Foliage/report.md).
     }
     f.jitter[2]=previous_frame_.jitter[0];f.jitter[3]=previous_frame_.jitter[1];
     f.light=light_matrix();f.eye={eye[0],eye[1],eye[2],1};
     const auto& lighting=scene.lighting;
     auto sun=lighting.source_values?lighting.sun_direction:std::array<float,3>{.48f,.78f,.4f};
     f.sun={sun[0],sun[1],sun[2],lighting.source_values?1.f:0.f};
     f.ambient={lighting.ambient_multiplier[0],lighting.ambient_multiplier[1],lighting.ambient_multiplier[2],0};
     f.diffuse={lighting.diffuse_multiplier[0],lighting.diffuse_multiplier[1],lighting.diffuse_multiplier[2],0};
     f.fog_color={lighting.fog_color[0],lighting.fog_color[1],lighting.fog_color[2],lighting.fog_enabled?1.f:0.f};
     f.fog={lighting.fog_start,lighting.fog_end,lighting.lightmap_strength,lighting.sky_blend};
     f.sky_b={lighting.sky_b[0],lighting.sky_b[1],lighting.sky_b[2],0};
     f.sky_c={lighting.sky_c[0],lighting.sky_c[1],lighting.sky_c[2],0};
    // Environment/source shading jumps invalidate all history; camera and skins do not. The day clock moves the
    // Sun/ambient/fog a little every game minute: resetting on each step re-noised the whole RT image every few
    // seconds, so gradual drift (2% per frame) is left to the resolve's variance clip and the deterministic Sun ray.
    const auto jumped=[](const std::array<float,4>& a,const std::array<float,4>& b) {
        for(std::size_t i=0;i<4;++i)if(std::abs(a[i]-b[i])>.02f*std::max({1.f,std::abs(a[i]),std::abs(b[i])}))return true;
        return false;
    };
    if(frame_&&(jumped(f.sun,previous_frame_.sun)||jumped(f.ambient,previous_frame_.ambient)||
       jumped(f.diffuse,previous_frame_.diffuse)||jumped(f.fog_color,previous_frame_.fog_color)||
       jumped(f.fog,previous_frame_.fog)||jumped(f.sky_b,previous_frame_.sky_b)||jumped(f.sky_c,previous_frame_.sky_c)))
        frame_=0;
     f.params={original_lighting_?0.f:1.f,options_.exposure,
         denoise_enabled_&&frame_>0?1.f:0.f,options_.antialiasing&&!fsr_?1.f:0.f}; // FSR replaces the FXAA resolve
     f.viewport={float(render_extent_.width),float(render_extent_.height),float(total_frames_%16777216u),denoise_enabled_?1.f:0.f};
     const float tangent=std::tan(options_.camera.fov*.008726646259971648f);
     f.camera_right={basis.right[0],basis.right[1],basis.right[2],tangent*float(extent_.width)/float(extent_.height)};
     f.camera_up={basis.up[0],basis.up[1],basis.up[2],tangent};
     f.camera_forward={basis.forward[0],basis.forward[1],basis.forward[2],0};
     f.previous_vp=previous_frame_.vp;f.previous_eye=previous_frame_.eye;
     f.previous_forward=previous_frame_.camera_forward;
     // w: original fullscreen gamma ramp 0x467030(1,1,1.2) (post.frag/overlay.frag); D3D9 SetGammaRamp is fullscreen-only.
     f.source_time={float(source_ticks),float(source_ticks)*.001f,grayscale_?1.f:0.f,
         (SDL_GetWindowFlags(window_)&SDL_WINDOW_FULLSCREEN)?1.f:0.f};
     uniform_offset_=slot*uniform_stride_;
     std::memcpy(static_cast<std::byte*>(uniform_.mapped)+uniform_offset_,&f,sizeof(f));
     unsigned current=frame_&1u;
     // Replacement shadows share color visibility; this is not evidence of original source shadow behavior.
     begin_pass(command_,shadow_pass_,shadow_fb_,{2048,2048},true);
     // Only enhanced raster lighting samples the shadow map (lighting.glsl shadowVisibility); original appearance and
     // the RT shaders never read it, so the pass only clears it.
     if(!options_.original_appearance&&!rt_enabled_)draw_scene(command_,true);
     vkCmdEndRenderPass(command_);
     begin_pass(command_,scene_pass_,scene_fb_[current],render_extent_,true);
    draw_scene(command_,false);
    // 0046ccf0 draws non-AfterHDR particles with the transparent list (glow input), then the hilite; 0046cbe0 draws
    // AfterHDR particles after the glow composite (0046fdb0 @470aed/@47183f).
    const auto draw_particles=[&](bool after_hdr) {
        bool bound=false;
        for(const auto& batch:particle_batches_) {
            if(batch.after_hdr!=after_hdr)continue;
            if(!bound){VkDeviceSize offset=0;vkCmdBindVertexBuffers(command_,0,1,&particle_buffer_[particle_slot_].handle,&offset);bound=true;}
            // Original 0049a780: depth test without write, per-system blend and D3DRS_DEPTHBIAS.
            vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,(after_hdr?after_particle_pipeline_:particle_pipeline_)[static_cast<unsigned>(batch.blend)]);
            bind_set(particle_sets_[batch.texture]);
            Draw draw{};draw.flags={batch.depth_bias,batch.alpha_reference,0,0};
            vkCmdPushConstants(command_,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
            vkCmdDraw(command_,batch.count,1,batch.first,0);
        }
    };
    draw_particles(false);
    if(!contours_.empty()) {
        // Contour draw 0047e1xx: shells i=3,2,1 with c8 = (colour, sin(tick*.004)*.015 + i*.02).
        vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,contour_pipeline_);
        const float pulse=std::sin(float(source_ticks)*.004f)*.015f;
        for(int shell=3;shell>=1;--shell)for(const auto& item:contours_) {
            Draw draw{};draw.model=item.model;draw.color={item.color[0],item.color[1],item.color[2],pulse+float(shell)*.02f};
            // Same frame-uniform set as the instance's own scene draw; contour shaders read no images.
            bind_set(material_sets_[item.material<material_sets_.size()?item.material:0]);
            vkCmdPushConstants(command_,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
            auto& g=geometry_[item.mesh];VkDeviceSize offset=0;
            vkCmdBindVertexBuffers(command_,0,1,&g.vertices.handle,&offset);
            vkCmdBindIndexBuffer(command_,g.indices.handle,0,VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command_,g.count,1,0,0,0);
        }
    }
    vkCmdEndRenderPass(command_);
     if(denoise_enabled_) {
         // Temporal accumulation (stride 0), then a-trous strides 1/2/4: history -> A -> B -> A.
         vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,temporal_pipeline_);
         for(unsigned k=0;k<4;++k) {
             begin_pass(command_,temporal_pass_,k?atrous_fb_[current][k==2]:temporal_fb_[current],render_extent_,false);
             bind_set(k?atrous_sets_[current][k-1]:temporal_sets_[current]);
             Draw draw{};draw.color={k?float(1u<<(k-1)):0.f,0,0,0};
             vkCmdPushConstants(command_,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
             vkCmdDraw(command_,3,1,0,0);vkCmdEndRenderPass(command_);
         }
     }
     // Original fake HDR pass (0046fdb0 @470d2f) at 128x128, same chain as the GL renderer: half_intensity bb3c =
     // .45*scene + .55*bb38 (the previous frame's chain), then vertex buffers 0x77c344 (v) and 0x77c358 (u),
     // each 2x bb3c->bb38->bb3c of t_pseudo_HDR_blur, composited onto the scene before AfterHDR particles.
     const float glow_gain=scene.lighting.glow*.02f;
     const auto glow_step=[&](unsigned set,unsigned target,float dx,float dy,float mode) {
         begin_pass(command_,glow_pass_,glow_fb_[target],{128,128},false);
         bind_set(glow_sets_[set]);
         vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,glow_pipeline_);
         Draw draw{};draw.color={dx,dy,glow_gain,mode};
         vkCmdPushConstants(command_,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
         vkCmdDraw(command_,3,1,0,0);vkCmdEndRenderPass(command_);
     };
     if(glow_pass_) {
         glow_step(current,1,0,0,0);
         for(int axis=0;axis<2;++axis)for(int i=0;i<2;++i) {
             glow_step(2,0,axis?1.f/128:0,axis?0:1.f/128,1);
             glow_step(3,1,axis?1.f/128:0,axis?0:1.f/128,1);
         }
     }
     if(glow_pass_||std::any_of(particle_batches_.begin(),particle_batches_.end(),[](const ParticleBatch& b){return b.after_hdr;})) {
         begin_pass(command_,after_pass_,after_fb_[current],render_extent_,false);
         if(glow_pass_) { // 0046fdb0 @4716a2: quad diffuse 0xffcccccc, SRCCOLOR/ONE.
             bind_set(glow_sets_[2]);
             vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,glow_composite_pipeline_);
             Draw draw{};draw.color={0,0,glow_gain,2};
             vkCmdPushConstants(command_,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(draw),&draw);
             vkCmdDraw(command_,3,1,0,0);
         }
         draw_particles(true);
         vkCmdEndRenderPass(command_);
     }
    float frame_ms=16.6667f;bool fsr_reset_frame=false;
    if(fsr_) {
         // FSR 3.1.4 resolves the jittered render-resolution frame into fsr_output_ at display size, before tonemap/HUD.
         // The depth transition also orders the previous frame's post-pass read of fsr_output_ before FSR rewrites it.
         constexpr VkPipelineStageFlags depth_stages=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
         barrier(command_,depth_.handle,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,depth_stages,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
         const auto now=std::chrono::steady_clock::now();
         if(last_render_!=std::chrono::steady_clock::time_point{})frame_ms=std::chrono::duration<float,std::milli>(now-last_render_).count();
         last_render_=now;
         const auto input=[](const Image& i,VkImageUsageFlags usage){return Fsr3Upscaler::Image{i.handle,i.format,{i.width,i.height},usage};};
         constexpr auto target=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
         fsr_->dispatch({command_,input(hdr_[current],target|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT),
             input(depth_,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT),input(motion_,target),input(reactive_,target),
             input(fsr_output_,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT),fsr_jitter,
             std::clamp(frame_ms,.1f,1000.f),options_.fsr_sharpness,options_.camera.near_plane,options_.camera.far_plane,
             options_.camera.fov*.017453292519943295f,fsr_reset_});
         fsr_reset_frame=fsr_reset_;fsr_reset_=false;
         barrier(command_,depth_.handle,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
             0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,depth_stages,VK_IMAGE_ASPECT_DEPTH_BIT);
         VkMemoryBarrier upscaled{};upscaled.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
         upscaled.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;upscaled.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
         vkCmdPipelineBarrier(command_,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&upscaled,0,nullptr,0,nullptr);
     }
     begin_pass(command_,post_pass_,post_fb_,extent_,false);
     bind_set(post_sets_[current]);
     vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,post_pipeline_);vkCmdDraw(command_,3,1,0,0);
     if(overlay_visible_&&!generating_) {
         vkCmdBindPipeline(command_,VK_PIPELINE_BIND_POINT_GRAPHICS,overlay_pipeline_);vkCmdDraw(command_,3,1,0,0);
     }
     vkCmdEndRenderPass(command_);
     auto& frame=slots_[slot];
     VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;
     VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
     submit.commandBufferCount=1;submit.pCommandBuffers=&frame.command;
     VkResult result=VK_SUCCESS;
     if(generating_) {
         // The frame halfway to this one (unless history was reset: then only this frame is shown).
         const bool generated=!fsr_reset_frame&&generation_frame_!=0;
         record_generation(slot,current,std::clamp(frame_ms,.1f,1000.f),!generated);
         checked(vkEndCommandBuffer(command_),"end frame");
         // generated_/real_[slot] were last read by the present job of frame total_frames_-2.
         drain_presents(1);
         checked(vkResetFences(device_,1,&frame.fence),"reset frame fence");
         checked(queue_submit(submit,frame.fence),"submit frame");
         {
             std::scoped_lock lock(present_mutex_);
             present_jobs_.push_back({slot,generated,frame.fence,std::chrono::steady_clock::now()});++present_queued_;
         }
         present_cv_.notify_all();generated_last_=generated;
     } else {
         // Source stage = the acquire semaphore's wait stage, so the layout transition happens after presentation released it.
         barrier(command_,swap_images_[image_index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
         VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
         copy.extent={extent_.width,extent_.height,1};
         vkCmdCopyImage(command_,output_.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swap_images_[image_index],
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
         barrier(command_,swap_images_[image_index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
             VK_ACCESS_TRANSFER_WRITE_BIT,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
         checked(vkEndCommandBuffer(command_),"end frame");
         checked(vkResetFences(device_,1,&frame.fence),"reset frame fence");
         submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&frame.acquire;submit.pWaitDstStageMask=&wait;
         submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&present_semaphores_[image_index];
         VkPresentInfoKHR present{};present.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
         present.waitSemaphoreCount=1;present.pWaitSemaphores=&present_semaphores_[image_index];
         present.swapchainCount=1;present.pSwapchains=&swapchain_;present.pImageIndices=&image_index;
         std::scoped_lock lock(queue_mutex_);
         checked(vkQueueSubmit(queue_,1,&submit,frame.fence),"submit frame");
         result=vkQueuePresentKHR(queue_,&present);
     }
     frame.pending=true;frame.serial=total_frames_;particle_drawn_[particle_slot_]=total_frames_;
     if(result==VK_ERROR_OUT_OF_DATE_KHR||result==VK_SUBOPTIMAL_KHR)dirty_=true;
     else checked(result,"present frame");
     // With frame generation the present thread presents this frame once the GPU finished it.
     if(!source_presented_&&scene.source_registry_id.has_value()&&!scene.meshes.empty()&&
        (result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR)) {
         std::cout<<"Source presented source_registry_id="<<*scene.source_registry_id<<" provider=vulkan\n"<<std::flush;
         source_presented_=true;
     }
     if(denoise_enabled_) {
         if(f.params[2]>0){++history_eligible_frames_;if(scene_changed)++history_retained_dirty_frames_;}
         else ++history_reset_events_;
     }
     previous_frame_=f;rendered_=true;++frame_;
     ++total_frames_;
     if(total_frames_<=2||total_frames_%300==0) {
         const bool active=rt_enabled_&&!original_lighting_;
         // Rendered (submitted) frame period since the previous line: the FIFO-paced frame rate without frame generation.
         const auto now=std::chrono::steady_clock::now();
         const double frame_ms_avg=log_frame_?std::chrono::duration<double,std::milli>(now-log_time_).count()/double(total_frames_-log_frame_):0;
         log_time_=now;log_frame_=total_frames_;
         std::cerr<<"Vulkan submitted frame="<<total_frames_<<" provider=vulkan appearance="
            <<(options_.original_appearance?"original":"enhanced")<<" rtStyle="<<(options_.rt_style==RtStyle::faithful?"faithful":"physical")
            <<" lighting="<<(original_lighting_?"original":"physical")<<" hardwareRayQueries="<<rt_enabled_
            <<" accelerationStructure="<<(rt_enabled_&&rt_->acceleration_structure()!=VK_NULL_HANDLE)
            <<" reflections="<<active<<" shadows="<<rt_enabled_<<" diffuseGI="<<active<<" renderedFrameMs="<<frame_ms_avg
             <<" historyEligible="<<int(f.params[2])<<" historyEligibleFrames="<<history_eligible_frames_
             <<" historyResetEvents="<<history_reset_events_<<" historyRetainedDirtyFrames="<<history_retained_dirty_frames_
             <<" historySampleCap=32 geometryMotionVectors="<<motion_vectors_<<" denoise="<<denoise_enabled_<<" renderScale="<<options_.render_scale
               <<" upscaler="<<(fsr_?"fsr3.1.4":"bilinear")<<" fsrQuality="<<(fsr_?static_cast<int>(options_.fsr_quality):-1)
               <<" frameGeneration="<<generating_
                <<" renderExtent="<<render_extent_.width<<'x'<<render_extent_.height<<" outputExtent="<<extent_.width<<'x'<<extent_.height<<'\n';
     }
 }
 // One display-size image in TRANSFER_SRC_OPTIMAL (output_/real_/generated_) to an RGB file.
 void write_display(const Image& source,const std::filesystem::path& path) {
     auto readback=buffer(VkDeviceSize(extent_.width)*extent_.height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
     try {
         immediate([&](VkCommandBuffer c) {
             VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent_.width,extent_.height,1};
             vkCmdCopyImageToBuffer(c,source.handle,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.handle,1,&copy);
             VkBufferMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
             b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.buffer=readback.handle;b.size=VK_WHOLE_SIZE;
             vkCmdPipelineBarrier(c,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&b,0,nullptr);
         });
         const auto* pixels=static_cast<const std::uint8_t*>(readback.mapped);bool bgra=swap_format_==VK_FORMAT_B8G8R8A8_UNORM;
         std::vector<std::uint8_t> rgb(std::size_t(extent_.width)*extent_.height*3);
         for(std::uint32_t y=0;y<extent_.height;++y) {
             progress();
             for(std::uint32_t x=0;x<extent_.width;++x) {
                 const auto* p=pixels+(std::size_t(y)*extent_.width+x)*4;auto* o=rgb.data()+(std::size_t(y)*extent_.width+x)*3;
                 o[0]=p[bgra?2:0];o[1]=p[1];o[2]=p[bgra?0:2];
             }
         }
         write_rgb_image(path,int(extent_.width),int(extent_.height),rgb);
     } catch(...){destroy(readback);throw;}
     destroy(readback);
 }
 // With frame generation also <stem>-fg-previous / -fg-generated: the previous rendered frame and the generated frame
 // shown between it and this one (both with HUD), for interpolation inspection.
 void screenshot(const std::filesystem::path& path)override {
     if(!rendered_)throw std::runtime_error("Vulkan screenshot requires a rendered frame");
     wait_frame("screenshot frame wait");
     std::cerr<<"Vulkan completed frames="<<total_frames_<<" provider=vulkan hardwareRayQueries="<<rt_enabled_<<'\n';
     write_display(output_,path);
     if(!generating_||generation_frame_<2)return;
     const auto sibling=[&](const char* suffix){return path.parent_path()/(path.stem().string()+suffix+path.extension().string());};
     const unsigned last=unsigned(total_frames_-1)&1u;
     write_display(real_[last^1u],sibling("-fg-previous"));write_display(generated_[last],sibling("-fg-generated"));
     std::cerr<<"Vulkan frame generation screenshot generatedValid="<<generated_last_<<'\n';
 }
};
} std::unique_ptr<Renderer> make_vk_renderer(SDL_Window* window,const RendererOptions& options){return std::make_unique<VulkanRenderer>(window,options);} }
