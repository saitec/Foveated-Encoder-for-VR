#include "oculus2.hpp"

#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <thread>
#include <iomanip>

#include "../optimizer/optimizer.hpp"
#include "../settings.hpp"

using cv::Mat;

static int init(void);
static void cleanup(void);
static void toggle_hmd_fullscreen(void);
static void display();
static void updatePipelineOrientation(OptimizerPipeline& pipeline, double offsetIntoFuture);
static void draw_scene(ovrEyeType);
static void update_rtarg(int width, int height);
static int handle_event(SDL_Event *ev);
static int key_event(int key, int state);
static void reshape(int x, int y);
static unsigned int next_pow2(unsigned int x);
static void quat_to_matrix(const float *quat, float *mat);

static SDL_Window *win;
static SDL_GLContext ctx;
static int win_width, win_height;

static unsigned int fbo, fb_tex, fb_depth;
static int fb_width, fb_height;
static int fb_tex_width, fb_tex_height;

static ovrHmd hmd;
static ovrSizei eyeres[2];
static ovrEyeRenderDesc eye_rdesc[2];
static ovrGLTexture fb_ovr_tex[2];
static union ovrGLConfig glcfg;
static unsigned int distort_caps;
static unsigned int hmd_caps;

static GLUquadric* qobj;

static float xPos = 0, yPos = 0, zPos = 0, ourAngle = 0;

static TextureData textureLeft;
static TextureData textureRight;

static bool three_d_enabled = true;
static bool FROZEN = false;
const static int ROTATION_GRANULARITY = 20;

static float OculusZAngle = 0;
static float OculusPitchAngle = 0;

static GLuint myDisplayList;
static bool UsePrediction = true;

static inline float getHorizontalAngleForOptimize() {
  return -(OculusZAngle + ourAngle) + 180;
}

static inline float getVerticalAngleForOptimize() {
  return 90 - PITCH_MULTIPLIER * OculusPitchAngle;
}

static FramerateProfiler videoReadProfiler;
static FramerateProfiler loadTextureProfiler;
static FramerateProfiler displayProfiler;
static FramerateProfiler sdlProfiler;
static FramerateProfiler optimizeProfiler;
static FramerateProfiler glTextureProfiler;
static RollingAverage optimizeAverage;

// TextureData

TextureData::TextureData() {
}

void TextureData::init() {
  REQUIRES(!initialized);

  glGenTextures(1, &this->name);
  glGenBuffers(1, &this->pbo);

  glBindTexture(GL_TEXTURE_2D, this->name);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glBindTexture(GL_TEXTURE_2D, 0);

  initialized = true;
}

void TextureData::load(const Mat& input) {
  glBindTexture(GL_TEXTURE_2D, this->name);

  Mat image;
#ifdef USE_OPTIMIZER_PIPELINE
  image = input;
#else
  if (USE_OPTIMIZER) {
    optimizeProfiler.startFrame();
    image = Optimizer::processImage(input, getHorizontalAngleForOptimize(), getVerticalAngleForOptimize());
    optimizeProfiler.endFrame();
  } else {
    image = input;
  }
#endif

  if (loaded) {
    // The texture size should stay the same
    REQUIRES(image.cols == this->width);
    REQUIRES(image.rows == this->height);

#ifdef USE_PIXEL_BUFFER
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->pbo);

    // map the buffer object into client's memory
    // Note that glMapBuffer() causes sync issue.
    // If GPU is working with this buffer, glMapBuffer() will wait(stall)
    // for GPU to finish its job. To avoid waiting (stall), you can call
    // first glBufferData() with NULL pointer before glMapBuffer().
    // If you do that, the previous data in PBO will be discarded and
    // glMapBuffer() returns a new allocated pointer immediately
    // even if GPU is still working with the previous data.

     glBufferData(GL_PIXEL_UNPACK_BUFFER, this->size, NULL, GL_STREAM_DRAW);
     GLubyte* ptr = (GLubyte*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
     std::memcpy(ptr, image.ptr(), this->size);
     glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER); // release pointer to mapping buffer

    // copy via pixel buffer
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, this->width, this->height, GL_BGR, GL_UNSIGNED_BYTE, 0);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#else
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, this->width, this->height, GL_BGR, GL_UNSIGNED_BYTE, image.ptr());
#endif

  } else {
    // The first video frame we get, intiialize the texture

    this->height = image.rows;
    this->width = image.cols;
    this->size = width * height * 3; // RGB = 3 channels, 1 byte each

    // Initialize Texture
    glTexImage2D(GL_TEXTURE_2D, 0, 3, this->width, this->height, 0,
        GL_BGR, GL_UNSIGNED_BYTE, image.ptr());

#ifdef USE_PIXEL_BUFFER
    // Initialize pixel buffer objects, need to delete them when program exits.
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->pbo);
    // glBufferData with NULL pointer only reserves memory space.
    glBufferData(GL_PIXEL_UNPACK_BUFFER, this->size, NULL, GL_STREAM_DRAW);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
#endif

    loaded = true;
  }

  glBindTexture(GL_TEXTURE_2D, 0);
}

#define CHECK_GL_ERROR() checkGLError(__FILE__, __LINE__)

static void checkGLError(const char *file, int line) {
  GLenum glErr;

  glErr = glGetError();
  if (glErr != GL_NO_ERROR)
  {
      printf("glError in file %s @ line %d: %s\n",
             file, line, gluErrorString(glErr));
      ASSERT(false);
  }
}

#ifdef USE_OPTIMIZER_PIPELINE
static double updateVideoFrame(OptimizerPipeline& pipeline, bool firstFrame) {

  // on the first frame, we want to wait till the video starts
  if (!firstFrame && !pipeline.isFrameAvailable()) {
    glTextureProfiler.startFrame();
    glTextureProfiler.endFrame();
    videoReadProfiler.startFrame();
    videoReadProfiler.endFrame();
    return -1;
  }

  if (!firstFrame)
    videoReadProfiler.startFrame();

  FrameData fd = pipeline.getFrame();
  Mat image = fd.image;
  optimizeAverage.addSample(fd.optimizeTime);

  if (!firstFrame)
    videoReadProfiler.endFrame();

  glTextureProfiler.startFrame();
  cv::Mat left = cv::Mat(image, cv::Range(0, image.rows / 2));
  cv::Mat right = cv::Mat(image, cv::Range(image.rows / 2, image.rows));
  textureLeft.load(left);
  textureRight.load(right);
  glTextureProfiler.endFrame();

  return fd.timestamp;
}
#else
static double updateVideoFrame(VideoReader& videoReader, bool firstFrame) {
  // on the first frame, we want to wait till the video starts
  if (!firstFrame && !videoReader.isFrameAvailable())
    return -1;

  if (!firstFrame)
    videoReadProfiler.startFrame();
  cv::Mat image = videoReader.getFrame();
  if (!firstFrame)
    videoReadProfiler.endFrame();

  loadTextureProfiler.startFrame();
  cv::Mat left = cv::Mat(image, cv::Range(0, image.rows / 2));
  cv::Mat right = cv::Mat(image, cv::Range(image.rows / 2, image.rows));
  textureLeft.load(left);
  textureRight.load(right);
  loadTextureProfiler.endFrame();

  return -1;
}
#endif

int Oculus2::run(int argc, char **argv)
{
  if (argc < 3) {
    std::cerr << "Usage: "
      << argv[0]
      << " rendertest filename [secondsToRun]"
      << std::endl;
      return 1;
  }

  int secondsToRun = -1;
  if (argc >= 4) {
    secondsToRun = atoi(argv[3]);
    std::cout << "time limit " << secondsToRun << " seconds\n";
  }

  std::cout
    << "Q/E: manually rotate left/right\n"
    << "R/Space: recenter\n"
    << "B: high blur\n"
    << "N: no blur\n"
    << "G: 3D\n"
    << "T: timewarp\n"
    << "P: pause video updates\n"
    << "+/- (keypad): increase/decrease blur\n"
    << "U: use prediction\n"
    << "V: fovea\n"
    // << "o: toggle OLED overdrive (default: on)\n"
    // << "l: toggle low persistence display (default: on)\n"
    // << "v: toggle vignette (default: on)\n"
  ;

  if (init() == -1) {
    return 1;
  }

  std::string filename = argv[2];
  VideoReader myVideoReader(filename);

#ifdef USE_OPTIMIZER_PIPELINE
  OptimizerPipeline pipeline(&myVideoReader);
#endif

  textureLeft.init();
  textureRight.init();

  myDisplayList = glGenLists(1);
  glNewList(myDisplayList, GL_COMPILE);
  gluCylinder(qobj, 10.0, 10.0, 20.0, 20, 20);
  glEndList();

  FramerateProfiler profiler;
  double lastFPSAnnouncement = Timer::timeInSeconds();

  RollingAverage mtpProfiler;
  RollingAverage vrfc;
  RollingAverage ofc;

  bool isFirstFrame = true;
  double totalRunStart = Timer::timeInSeconds();

  while (true) {
    profiler.startFrame();

    double timestamp = -1;
    if (!FROZEN) {
#ifdef USE_OPTIMIZER_PIPELINE
      timestamp = updateVideoFrame(pipeline, isFirstFrame);
#else
      timestamp = updateVideoFrame(myVideoReader, isFirstFrame);
#endif

      isFirstFrame = false;
    }

    if (timestamp > 0) {
      double mtpTime = Timer::timeInSeconds() - timestamp;
      ASSERT(mtpTime > 0);
      mtpProfiler.addSample(mtpTime);
    }

    vrfc.addSample(myVideoReader.getNumFramesAvailable());
    ofc.addSample(pipeline.getNumFramesAvailable());

    displayProfiler.startFrame();
    display();
    displayProfiler.endFrame();
    double mtuTime = mtpProfiler.getAverage();
    updatePipelineOrientation(pipeline, mtuTime); // TODO: optimal placement?

    double now = Timer::timeInSeconds();
    if (now - lastFPSAnnouncement > 2) {
      lastFPSAnnouncement = now;

      std::cout.precision(2);
      std::cout
      << std::setw(7) << std::fixed << profiler.getFramerate() << " FPS = "
      << std::setw(7) << profiler.getAverageTimeMillis() << " ms/frame = "
      << std::setw(7) << displayProfiler.getAverageTimeMillis() << " (display)"
      // << " + " << std::setw(7) << loadTextureProfiler.getAverageTimeMillis() << " (loadTexture)"
      << " + " << std::setw(7) << glTextureProfiler.getAverageTimeMillis() << " (loadTexture)"
      << " + " << std::setw(7) << videoReadProfiler.getAverageTimeMillis() << " (readVideo)"
      << ";    M2U=" << std::setw(7) << 1000 * mtuTime << "    "
      << "optimize=" << std::setw(7) << 1000 * optimizeAverage.getAverage() << "    "
      << "VRQ=" << std::setw(5) << vrfc.getAverage() << "    "
      << "OQ=" << std::setw(5) << ofc.getAverage() << "    "
      << std::endl;
    }

    if (secondsToRun > 0 && now - totalRunStart > secondsToRun)
      goto done;

    sdlProfiler.startFrame();
    SDL_Event ev;
    while(SDL_PollEvent(&ev)) {
      if(handle_event(&ev) == -1) {
        goto done;
      }
    }
    sdlProfiler.endFrame();

    profiler.endFrame();
  }

done:
  std::cout.precision(2);
  std::cout
  << "\n========== Lifetime Stats ==========\n"
  << "Total time: " << (Timer::timeInSeconds() - totalRunStart) << "s\n"
  << std::setw(7) << std::fixed << profiler.getLifetimeFramerate() << " FPS = "
  << std::setw(7) << profiler.getLifetimeAverageMillis() << " = "
  << std::setw(7) << displayProfiler.getLifetimeAverageMillis() << " (display)"
  // << " + " << std::setw(7) << loadTextureProfiler.getAverageTimeMillis() << " (loadTexture)"
  << " + " << std::setw(7) << glTextureProfiler.getLifetimeAverageMillis() << " (loadTexture)"
  << " + " << std::setw(7) << videoReadProfiler.getLifetimeAverageMillis() << " (readVideo)"
  << ";    M2U=" << std::setw(7) << 1000 * mtpProfiler.getLifetimeAverage() << "    "
  << "VRQ=" << std::setw(5) << vrfc.getLifetimeAverage() << "    "
  << "OQ=" << std::setw(5) << ofc.getLifetimeAverage()
  << std::endl;

  cleanup();
  return 0;
}


int init(void)
{
  int i, x, y;
  unsigned int flags;

  /* libovr must be initialized before we create the OpenGL context */
  if (!ovr_Initialize(0)) {
    std::cerr << "Unable to initialize OVR" << std::endl;
  }

  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);

  x = y = SDL_WINDOWPOS_UNDEFINED;
  flags = SDL_WINDOW_OPENGL;
  if(!(win = SDL_CreateWindow("Conduit", x, y, 1024, 640, flags))) {
    fprintf(stderr, "failed to create window\n");
    return -1;
  }
  if(!(ctx = SDL_GL_CreateContext(win))) {
    fprintf(stderr, "failed to create OpenGL context\n");
    return -1;
  }

  glewInit();

  bool is_debug = false;
  if(!(hmd = ovrHmd_Create(0))) {
    fprintf(stderr, "failed to open Oculus HMD, falling back to virtual debug HMD\n");
    is_debug = true;
    if(!(hmd = ovrHmd_CreateDebug(ovrHmd_DK2))) {
      fprintf(stderr, "failed to create virtual debug HMD\n");
      return -1;
    }
  }
  printf("initialized HMD: %s - %s\n", hmd->Manufacturer, hmd->ProductName);

  /* resize our window to match the HMD resolution */
  win_width = hmd->Resolution.w;
  win_height = hmd->Resolution.h;
  if (is_debug) {
    win_width /= 2;
    win_height /= 2;
  }

  SDL_SetWindowSize(win, win_width, win_height);
  SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

  /* enable position and rotation tracking */
  ovrHmd_ConfigureTracking(hmd, ovrTrackingCap_Orientation | ovrTrackingCap_MagYawCorrection | ovrTrackingCap_Position, 0);
  /* retrieve the optimal render target resolution for each eye */
  eyeres[0] = ovrHmd_GetFovTextureSize(hmd, ovrEye_Left, hmd->DefaultEyeFov[0], 1.0);
  eyeres[1] = ovrHmd_GetFovTextureSize(hmd, ovrEye_Right, hmd->DefaultEyeFov[1], 1.0);

  /* and create a single render target texture to encompass both eyes */
  fb_width = eyeres[0].w + eyeres[1].w;
  fb_height = eyeres[0].h > eyeres[1].h ? eyeres[0].h : eyeres[1].h;
  update_rtarg(fb_width, fb_height);

  /* fill in the ovrGLTexture structures that describe our render target texture */
  for(i=0; i<2; i++) {
    fb_ovr_tex[i].OGL.Header.API = ovrRenderAPI_OpenGL;
    fb_ovr_tex[i].OGL.Header.TextureSize.w = fb_tex_width;
    fb_ovr_tex[i].OGL.Header.TextureSize.h = fb_tex_height;
    /* this next field is the only one that differs between the two eyes */
    fb_ovr_tex[i].OGL.Header.RenderViewport.Pos.x = i == 0 ? 0 : fb_width / 2.0;
    fb_ovr_tex[i].OGL.Header.RenderViewport.Pos.y = 0;
    fb_ovr_tex[i].OGL.Header.RenderViewport.Size.w = fb_width / 2.0;
    fb_ovr_tex[i].OGL.Header.RenderViewport.Size.h = fb_height;
    fb_ovr_tex[i].OGL.TexId = fb_tex; /* both eyes will use the same texture id */
  }

  /* fill in the ovrGLConfig structure needed by the SDK to draw our stereo pair
   * to the actual HMD display (SDK-distortion mode)
   */
  memset(&glcfg, 0, sizeof glcfg);
  glcfg.OGL.Header.API = ovrRenderAPI_OpenGL;
  glcfg.OGL.Header.BackBufferSize.w = win_width;
  glcfg.OGL.Header.BackBufferSize.h = win_height;
  glcfg.OGL.Header.Multisample = 1;

#ifdef OVR_OS_WIN32
  glcfg.OGL.Window = GetActiveWindow();
  glcfg.OGL.DC = wglGetCurrentDC();
#elif defined(OVR_OS_LINUX)
  glcfg.OGL.Disp = glXGetCurrentDisplay();
#endif

  if(hmd->HmdCaps & ovrHmdCap_ExtendDesktop) {
    printf("running in \"extended desktop\" mode\n");
  } else {
    /* to sucessfully draw to the HMD display in "direct-hmd" mode, we have to
     * call ovrHmd_AttachToWindow
     * XXX: this doesn't work properly yet due to bugs in the oculus 0.4.1 sdk/driver
     */
#ifdef WIN32
    ovrHmd_AttachToWindow(hmd, glcfg.OGL.Window, 0, 0);
#elif defined(OVR_OS_LINUX)
    ovrHmd_AttachToWindow(hmd, (void*)glXGetCurrentDrawable(), 0, 0);
#endif
    printf("running in \"direct-hmd\" mode\n");
  }

  /* enable low-persistence display and dynamic prediction for lattency compensation */
  hmd_caps = ovrHmdCap_LowPersistence | ovrHmdCap_DynamicPrediction;
  ovrHmd_SetEnabledCaps(hmd, hmd_caps);

  /* configure SDK-rendering and enable OLED overdrive and timewrap, which
   * shifts the image before drawing to counter any lattency between the call
   * to ovrHmd_GetEyePose and ovrHmd_EndFrame.
   */
  distort_caps = ovrDistortionCap_TimeWarp | ovrDistortionCap_Overdrive;
  if(!ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc)) {
    fprintf(stderr, "failed to configure distortion renderer\n");
  }

  glEnable(GL_DEPTH_TEST);
  glEnable(GL_CULL_FACE);
  glEnable(GL_NORMALIZE);
  glEnable(GL_TEXTURE_2D);

  glClearColor(0, 0, 0, 1);
  // glClearColor(79.0/255, 176.0/255, 225.0/255, 1);
  glColor3f(1, 1, 1);

  qobj = gluNewQuadric();
  gluQuadricNormals(qobj, GLU_SMOOTH);
  gluQuadricTexture(qobj, true);
  glFrontFace(GL_CW);

  if (!is_debug)
    toggle_hmd_fullscreen();

  return 0;
}

void cleanup(void)
{
  printf("Cleaning up...\n");

  if(hmd) {
    ovrHmd_Destroy(hmd);
  }
  ovr_Shutdown();

  SDL_Quit();
}

void toggle_hmd_fullscreen(void)
{
  static int fullscr, prev_x, prev_y;
  fullscr = !fullscr;

  if(fullscr) {
    /* going fullscreen on the rift. save current window position, and move it
     * to the rift's part of the desktop before going fullscreen
     */
    SDL_GetWindowPosition(win, &prev_x, &prev_y);
    SDL_SetWindowPosition(win, hmd->WindowsPos.x, hmd->WindowsPos.y);
    SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);

#ifdef OVR_OS_LINUX
    /* on linux for now we have to deal with screen rotation during rendering. The docs are promoting
     * not rotating the DK2 screen globally
     */
    glcfg.OGL.Header.BackBufferSize.w = hmd->Resolution.h;
    glcfg.OGL.Header.BackBufferSize.h = hmd->Resolution.w;

    distort_caps |= ovrDistortionCap_LinuxDevFullscreen;
    ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
#endif
  } else {
    /* return to windowed mode and move the window back to its original position */
    SDL_SetWindowFullscreen(win, 0);
    SDL_SetWindowPosition(win, prev_x, prev_y);

#ifdef OVR_OS_LINUX
    glcfg.OGL.Header.BackBufferSize = hmd->Resolution;

    distort_caps &= ~ovrDistortionCap_LinuxDevFullscreen;
    ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
#endif
  }
}

void updatePipelineOrientation(OptimizerPipeline& pipeline, double offsetIntoFuture) {
  REQUIRES(offsetIntoFuture >= 0);
  if (offsetIntoFuture >= 0.09)
    offsetIntoFuture = 0.09;
  if (!UsePrediction)
    offsetIntoFuture = 0;

  // Query the HMD for the current tracking state.
  ovrTrackingState ts  = ovrHmd_GetTrackingState(hmd, ovr_GetTimeInSeconds() + offsetIntoFuture);
  OVR::Quatf q(ts.HeadPose.ThePose.Orientation);
  float yaw = 0, pitch = 0, roll = 0;
  q.GetEulerAngles<OVR::Axis_Y, OVR::Axis_X, OVR::Axis_Z>(&yaw, &pitch, &roll);
  OculusZAngle = yaw * MATH_DOUBLE_RADTODEGREEFACTOR;
  OculusPitchAngle = pitch * MATH_DOUBLE_RADTODEGREEFACTOR;

  pipeline.hmdDataMutex.lock();
  pipeline.lastUpdated = Timer::timeInSeconds();
  pipeline.hAngle = getHorizontalAngleForOptimize();
  pipeline.vAngle = getVerticalAngleForOptimize();
  pipeline.hmdDataMutex.unlock();
}

void display()
{
  int i;
  ovrMatrix4f proj;
  ovrPosef pose[2];
  float rot_mat[16];

  /* the drawing starts with a call to ovrHmd_BeginFrame */
  ovrHmd_BeginFrame(hmd, 0);

  /* start drawing onto our texture render target */
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  /* for each eye ... */
  for(i=0; i<2; i++) {
    ovrEyeType eye = hmd->EyeRenderOrder[i];

    /* -- viewport transformation --
     * setup the viewport to draw in the left half of the framebuffer when we're
     * rendering the left eye's view (0, 0, width/2, height), and in the right half
     * of the framebuffer for the right eye's view (width/2, 0, width/2, height)
     */
    glViewport(eye == ovrEye_Left ? 0 : fb_width / 2, 0, fb_width / 2, fb_height);

    /* -- projection transformation --
     * we'll just have to use the projection matrix supplied by the oculus SDK for this eye
     * note that libovr matrices are the transpose of what OpenGL expects, so we have to
     * use glLoadTransposeMatrixf instead of glLoadMatrixf to load it.
     */
    proj = ovrMatrix4f_Projection(hmd->DefaultEyeFov[eye], 0.5, 500.0, 1);
    glMatrixMode(GL_PROJECTION);
    glLoadTransposeMatrixf(proj.M[0]);

    /* -- view/camera transformation --
     * we need to construct a view matrix by combining all the information provided by the oculus
     * SDK, about the position and orientation of the user's head in the world.
     */
    /* TODO: use ovrHmd_GetEyePoses out of the loop instead */
    pose[eye] = ovrHmd_GetHmdPosePerEye(hmd, eye);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glTranslatef(eye_rdesc[eye].HmdToEyeViewOffset.x,
        eye_rdesc[eye].HmdToEyeViewOffset.y,
        eye_rdesc[eye].HmdToEyeViewOffset.z);
    /* retrieve the orientation quaternion and convert it to a rotation matrix */
    quat_to_matrix(&pose[eye].Orientation.x, rot_mat);
    glMultMatrixf(rot_mat);
    /* translate the view matrix with the positional tracking */
    glTranslatef(-pose[eye].Position.x, -pose[eye].Position.y, -pose[eye].Position.z);
    /* move the camera to the eye level of the user */
    glTranslatef(0, -ovrHmd_GetFloat(hmd, OVR_KEY_EYE_HEIGHT, 1.65), 0);
    glRotatef(ourAngle, 0, -1, 0);
    glTranslatef(xPos, yPos, zPos);

    /* finally draw the scene for this eye */
    draw_scene(eye);
  }

  /* after drawing both eyes into the texture render target, revert to drawing directly to the
   * display, and we call ovrHmd_EndFrame, to let the Oculus SDK draw both images properly
   * compensated for lens distortion and chromatic abberation onto the HMD screen.
   */
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  ovrHmd_EndFrame(hmd, pose, &fb_ovr_tex[0].Texture);

  /* workaround for the oculus sdk distortion renderer bug, which uses a shader
   * program, and doesn't restore the original binding when it's done.
   */
  glUseProgram(0);

  CHECK_GL_ERROR();
}

void reshape(int x, int y)
{
  win_width = x;
  win_height = y;
}

void draw_scene(ovrEyeType eye)
{
  bool isLeft = eye == ovrEye_Left || !three_d_enabled;
  glBindTexture(GL_TEXTURE_2D, isLeft ? textureLeft.name : textureRight.name);

//  glMatrixMode(GL_MODELVIEW);
  glRotatef(90.0,1.0,0.0,0.0);
  glTranslatef(0,0,-11);

  glCallList(myDisplayList);

  glBindTexture(GL_TEXTURE_2D, 0);
}

/* update_rtarg creates (and/or resizes) the render target used to draw the two stero views */
void update_rtarg(int width, int height)
{
  if(!fbo) {
    /* if fbo does not exist, then nothing does... create every opengl object */
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &fb_tex);
    glGenRenderbuffers(1, &fb_depth);

    glBindTexture(GL_TEXTURE_2D, fb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, fbo);

  /* calculate the next power of two in both dimensions and use that as a texture size */
  fb_tex_width = next_pow2(width);
  fb_tex_height = next_pow2(height);

  /* create and attach the texture that will be used as a color buffer */
  glBindTexture(GL_TEXTURE_2D, fb_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fb_tex_width, fb_tex_height, 0,
      GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb_tex, 0);

  /* create and attach the renderbuffer that will serve as our z-buffer */
  glBindRenderbuffer(GL_RENDERBUFFER, fb_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, fb_tex_width, fb_tex_height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb_depth);

  if(glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    fprintf(stderr, "incomplete framebuffer!\n");
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  printf("created render target: %dx%d (texture size: %dx%d)\n", width, height, fb_tex_width, fb_tex_height);
}

int handle_event(SDL_Event *ev)
{
  switch(ev->type) {
    case SDL_QUIT:
      return -1;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
      if(key_event(ev->key.keysym.sym, ev->key.state == SDL_PRESSED) == -1) {
        return -1;
      }
      break;

    case SDL_WINDOWEVENT:
      if(ev->window.event == SDL_WINDOWEVENT_RESIZED) {
        reshape(ev->window.data1, ev->window.data2);
      }
      break;

    default:
      break;
  }

  return 0;
}

int key_event(int key, int state)
{
  if (!state)
    return 0;

  ovrHSWDisplayState hsw;
  ovrHmd_GetHSWDisplayState(hmd, &hsw);
  if(hsw.Displayed) {
    ovrHmd_DismissHSWDisplay(hmd);
    return 0;
  }

  switch( key ){
    case SDLK_LEFT:
    case SDLK_a:
      xPos -= 1;
      break;
    case SDLK_RIGHT:
    case SDLK_d:
      xPos += 1;
      break;
    case SDLK_UP:
    case SDLK_w:
      zPos += 1;
      break;
    case SDLK_DOWN:
    case SDLK_s:
      zPos -= 1;
      break;
    case SDLK_q:
      ourAngle += ROTATION_GRANULARITY;
      break;
    case SDLK_e:
      ourAngle -= ROTATION_GRANULARITY;
      break;

    case 27:
      return -1;

    case SDLK_KP_PLUS:
      BLUR_FACTOR += 1;
      std::cout << "blur " << BLUR_FACTOR << "\n";
      break;

    case SDLK_KP_MINUS:
      BLUR_FACTOR = BLUR_FACTOR > 1 ? BLUR_FACTOR - 1 : 1;
      std::cout << "blur " << BLUR_FACTOR << "\n";
      break;

    case 'u':
      UsePrediction = !UsePrediction;
      printf("usePrediction=%d\n", UsePrediction);
      break;

    case 'v':
      FOVEA_DISPLAY = !FOVEA_DISPLAY;
      printf("fovea=%d\n", FOVEA_DISPLAY);
      break;

    case 'b':
      if (BLUR_FACTOR == BLUR_HIGH)
        BLUR_FACTOR = BLUR_NORMAL;
      else
        BLUR_FACTOR = BLUR_HIGH;

      std::cout << "blur " << BLUR_FACTOR << "\n";
      break;

    case 'n':
      if (BLUR_FACTOR == BLUR_NONE)
        BLUR_FACTOR = BLUR_NORMAL;
      else
        BLUR_FACTOR = BLUR_NONE;

      std::cout << "blur " << BLUR_FACTOR << "\n";
      break;

    case ' ':
    case 'r':
      /* allow the user to recenter by pressing space */
      ovrHmd_RecenterPose(hmd);
      break;

    case 'p':
      FROZEN = !FROZEN;
      printf("Toggling frozen to %d\n", FROZEN);
      break;

    case 'g':
      three_d_enabled = !three_d_enabled;
      printf("Toggling 3D to %d\n", three_d_enabled);
      break;

    case 'f':
      /* press f to move the window to the HMD */
      toggle_hmd_fullscreen();
      break;

    // case 'v':
    //   distort_caps ^= ovrDistortionCap_Vignette;
    //   printf("Vignette: %s\n", distort_caps & ovrDistortionCap_Vignette ? "on" : "off");
    //   ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
    //   break;

    case 't':
      distort_caps ^= ovrDistortionCap_TimeWarp;
      printf("Time-warp: %s\n", distort_caps & ovrDistortionCap_TimeWarp ? "on" : "off");
      ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
      break;

    case 'o':
      distort_caps ^= ovrDistortionCap_Overdrive;
      printf("OLED over-drive: %s\n", distort_caps & ovrDistortionCap_Overdrive ? "on" : "off");
      ovrHmd_ConfigureRendering(hmd, &glcfg.Config, distort_caps, hmd->DefaultEyeFov, eye_rdesc);
      break;

    case 'l':
      hmd_caps ^= ovrHmdCap_LowPersistence;
      printf("Low-persistence display: %s\n", hmd_caps & ovrHmdCap_LowPersistence ? "on" : "off");
      ovrHmd_SetEnabledCaps(hmd, hmd_caps);
      break;

    default:
      break;
  }

  return 0;
}

unsigned int next_pow2(unsigned int x)
{
  x -= 1;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  return x + 1;
}

/* convert a quaternion to a rotation matrix */
void quat_to_matrix(const float *quat, float *mat)
{
  mat[0] = 1.0 - 2.0 * quat[1] * quat[1] - 2.0 * quat[2] * quat[2];
  mat[4] = 2.0 * quat[0] * quat[1] + 2.0 * quat[3] * quat[2];
  mat[8] = 2.0 * quat[2] * quat[0] - 2.0 * quat[3] * quat[1];
  mat[12] = 0.0f;

  mat[1] = 2.0 * quat[0] * quat[1] - 2.0 * quat[3] * quat[2];
  mat[5] = 1.0 - 2.0 * quat[0]*quat[0] - 2.0 * quat[2]*quat[2];
  mat[9] = 2.0 * quat[1] * quat[2] + 2.0 * quat[3] * quat[0];
  mat[13] = 0.0f;

  mat[2] = 2.0 * quat[2] * quat[0] + 2.0 * quat[3] * quat[1];
  mat[6] = 2.0 * quat[1] * quat[2] - 2.0 * quat[3] * quat[0];
  mat[10] = 1.0 - 2.0 * quat[0]*quat[0] - 2.0 * quat[1]*quat[1];
  mat[14] = 0.0f;

  mat[3] = mat[7] = mat[11] = 0.0f;
  mat[15] = 1.0f;
}
