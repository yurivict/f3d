#include "window_impl.h"

#include "camera_impl.h"
#include "engine.h"
#include "log.h"
#include "options.h"

#include "vtkF3DConfigure.h"

#include "vtkF3DGenericImporter.h"
#include "vtkF3DNoRenderWindow.h"
#include "vtkF3DRendererWithColoring.h"

#include <vtkCamera.h>
#include <vtkImageData.h>
#include <vtkImageExport.h>
#include <vtkPNGReader.h>
#include <vtkPointGaussianMapper.h>
#include <vtkRenderWindow.h>
#include <vtkRendererCollection.h>
#include <vtkVersion.h>
#include <vtkWindowToImageFilter.h>
#include <vtksys/SystemTools.hxx>

#if F3D_MODULE_EXTERNAL_RENDERING
#include <vtkExternalOpenGLRenderWindow.h>
#endif

#if defined(_WIN32)
#include <vtkWindows.h>
#endif

namespace f3d::detail
{
class window_impl::internals
{
public:
  explicit internals(const options& options)
    : Options(options)
  {
  }

  std::string GetCachePath()
  {
    // create directories if they do not exist
    vtksys::SystemTools::MakeDirectory(this->CachePath);

    return this->CachePath;
  }

  std::unique_ptr<camera_impl> Camera;
  vtkSmartPointer<vtkRenderWindow> RenWin;
  vtkSmartPointer<vtkF3DRenderer> Renderer;
  Type WindowType;
  const options& Options;
  bool Initialized = false;
  std::string CachePath;
};

//----------------------------------------------------------------------------
window_impl::window_impl(const options& options, Type type)
  : Internals(std::make_unique<window_impl::internals>(options))
{
  this->Internals->WindowType = type;
  if (type == Type::NONE)
  {
    this->Internals->RenWin = vtkSmartPointer<vtkF3DNoRenderWindow>::New();
  }
  else if (type == Type::EXTERNAL)
  {
#if F3D_MODULE_EXTERNAL_RENDERING
    this->Internals->RenWin = vtkSmartPointer<vtkExternalOpenGLRenderWindow>::New();
#else
    throw engine::no_window_exception(
      "Window type is external but F3D_MODULE_EXTERNAL_RENDERING is not enabled");
#endif
  }
  else
  {
    this->Internals->RenWin = vtkSmartPointer<vtkRenderWindow>::New();
    this->Internals->RenWin->SetOffScreenRendering(type == Type::NATIVE_OFFSCREEN);
    this->Internals->RenWin->SetMultiSamples(0); // Disable hardware antialiasing

#ifdef __ANDROID__
    // Since F3D_MODULE_EXTERNAL_RENDERING is not supported on Android yet, we need to call
    // this workaround. It makes vtkEGLRenderWindow external if WindowInfo is not nullptr.
    this->Internals->RenWin->SetWindowInfo("jni");
#endif
  }

  this->Internals->Camera = std::make_unique<detail::camera_impl>();
}

//----------------------------------------------------------------------------
window_impl::Type window_impl::getType()
{
  return this->Internals->WindowType;
}

//----------------------------------------------------------------------------
camera& window_impl::getCamera()
{
  // Make sure the camera (and the whole rendering stack)
  // is initialized before providing one.
  if (!this->Internals->Initialized)
  {
    this->Initialize(false);
  }

  return *this->Internals->Camera;
}

//----------------------------------------------------------------------------
int window_impl::getWidth() const
{
  return this->Internals->RenWin->GetSize()[0];
}

//----------------------------------------------------------------------------
int window_impl::getHeight() const
{
  return this->Internals->RenWin->GetSize()[1];
}

//----------------------------------------------------------------------------
window& window_impl::setSize(int width, int height)
{
  this->Internals->RenWin->SetSize(width, height);
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setPosition(int x, int y)
{
  if (this->Internals->RenWin->IsA("vtkCocoaRenderWindow"))
  {
    // vtkCocoaRenderWindow has a different behavior than other render windows
    // https://gitlab.kitware.com/vtk/vtk/-/issues/18681
    int* screenSize = this->Internals->RenWin->GetScreenSize();
    int* winSize = this->Internals->RenWin->GetSize();
    this->Internals->RenWin->SetPosition(x, screenSize[1] - winSize[1] - y);
  }
  else
  {
    this->Internals->RenWin->SetPosition(x, y);
  }
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setIcon(const unsigned char* icon, size_t iconSize)
{
  // SetIcon needs https://gitlab.kitware.com/vtk/vtk/-/merge_requests/7004
#if VTK_VERSION_NUMBER >= VTK_VERSION_CHECK(9, 0, 20200616)
  // XXX This code requires that the interactor has already been set on the render window
  vtkNew<vtkPNGReader> iconReader;
  iconReader->SetMemoryBuffer(icon);
  iconReader->SetMemoryBufferLength(iconSize);
  iconReader->Update();
  this->Internals->RenWin->SetIcon(iconReader->GetOutput());
#else
  // Silent noop
  (void)icon;
  (void)iconSize;
#endif
  return *this;
}

//----------------------------------------------------------------------------
window& window_impl::setWindowName(const std::string& windowName)
{
  this->Internals->RenWin->SetWindowName(windowName.c_str());
  return *this;
}

//----------------------------------------------------------------------------
point3_t window_impl::getWorldFromDisplay(const point3_t& displayPoint) const
{
  point3_t out = { 0.0, 0.0, 0.0 };
  double worldPt[4];
  this->Internals->Renderer->SetDisplayPoint(displayPoint.data());
  this->Internals->Renderer->DisplayToWorld();
  this->Internals->Renderer->GetWorldPoint(worldPt);

  constexpr double homogeneousThreshold = 1e-7;
  if (worldPt[3] > homogeneousThreshold)
  {
    out[0] = worldPt[0] / worldPt[3];
    out[1] = worldPt[1] / worldPt[3];
    out[2] = worldPt[2] / worldPt[3];
  }
  return out;
}

//----------------------------------------------------------------------------
point3_t window_impl::getDisplayFromWorld(const point3_t& worldPoint) const
{
  point3_t out;
  this->Internals->Renderer->SetWorldPoint(worldPoint[0], worldPoint[1], worldPoint[2], 1.0);
  this->Internals->Renderer->WorldToDisplay();
  this->Internals->Renderer->GetDisplayPoint(out.data());
  return out;
}

//----------------------------------------------------------------------------
window_impl::~window_impl()
{
  if (this->Internals->Renderer)
  {
    // The axis widget should be disabled before calling the renderer destructor
    // As there is a register loop if not
    this->Internals->Renderer->ShowAxis(false);
  }
}

//----------------------------------------------------------------------------
void window_impl::Initialize(bool withColoring)
{
  // Clear renderer if already present
  if (this->Internals->Renderer)
  {
    // Hide axis to make sure the renderer can be deleted if needed
    this->Internals->Renderer->ShowAxis(false);
    this->Internals->RenWin->RemoveRenderer(this->Internals->Renderer);
  }

  // Create the renderer only when needed
  vtkF3DRendererWithColoring* renWithColor =
    vtkF3DRendererWithColoring::SafeDownCast(this->Internals->Renderer);
  if (withColoring && !renWithColor)
  {
    this->Internals->Renderer = vtkSmartPointer<vtkF3DRendererWithColoring>::New();
  }
  else if (!withColoring && (renWithColor || !this->Internals->Renderer))
  {
    this->Internals->Renderer = vtkSmartPointer<vtkF3DRenderer>::New();
  }

  this->Internals->Renderer->SetCachePath(this->Internals->GetCachePath());

  this->Internals->Camera->SetVTKRenderer(this->Internals->Renderer);
  this->Internals->RenWin->AddRenderer(this->Internals->Renderer);
  this->Internals->Renderer->Initialize(this->Internals->Options.getAsString("scene.up-direction"));

#if defined(_WIN32)
  // On Windows, the Log window can get in front in some case, make sure the render window is on top
  // on initialization
  HWND f3dWindow = static_cast<HWND>(this->Internals->RenWin->GetGenericWindowId());
  BringWindowToTop(f3dWindow);
#endif

  this->Internals->Initialized = true;
}

//----------------------------------------------------------------------------
void window_impl::UpdateDynamicOptions()
{
  if (!this->Internals->Renderer)
  {
    // Renderer is missing, create a default one
    this->Initialize(false);
  }

  // Make sure lights are created before we take options into account
  this->Internals->Renderer->UpdateLights();

  this->Internals->Renderer->ShowAxis(this->Internals->Options.getAsBool("interactor.axis"));
  this->Internals->Renderer->SetUseTrackball(
    this->Internals->Options.getAsBool("interactor.trackball"));
  this->Internals->Renderer->SetInvertZoom(
    this->Internals->Options.getAsBool("interactor.invert-zoom"));

  this->Internals->Renderer->SetLineWidth(
    this->Internals->Options.getAsDouble("render.line-width"));
  this->Internals->Renderer->SetPointSize(
    this->Internals->Options.getAsDouble("render.point-size"));
  this->Internals->Renderer->ShowEdge(this->Internals->Options.getAsBool("render.show-edges"));
  this->Internals->Renderer->ShowTimer(this->Internals->Options.getAsBool("ui.fps"));
  this->Internals->Renderer->ShowFilename(this->Internals->Options.getAsBool("ui.filename"));
  this->Internals->Renderer->SetFilenameInfo(
    this->Internals->Options.getAsString("ui.filename-info"));
  this->Internals->Renderer->ShowMetaData(this->Internals->Options.getAsBool("ui.metadata"));
  this->Internals->Renderer->ShowCheatSheet(this->Internals->Options.getAsBool("ui.cheatsheet"));
  this->Internals->Renderer->ShowDropZone(this->Internals->Options.getAsBool("ui.dropzone"));
  this->Internals->Renderer->SetDropZoneInfo(
    this->Internals->Options.getAsString("ui.dropzone-info"));

  this->Internals->Renderer->SetUseRaytracing(
    this->Internals->Options.getAsBool("render.raytracing.enable"));
  this->Internals->Renderer->SetRaytracingSamples(
    this->Internals->Options.getAsInt("render.raytracing.samples"));
  this->Internals->Renderer->SetUseRaytracingDenoiser(
    this->Internals->Options.getAsBool("render.raytracing.denoise"));

  this->Internals->Renderer->SetUseSSAOPass(
    this->Internals->Options.getAsBool("render.effect.ambient-occlusion"));
  this->Internals->Renderer->SetUseFXAAPass(
    this->Internals->Options.getAsBool("render.effect.anti-aliasing"));
  this->Internals->Renderer->SetUseToneMappingPass(
    this->Internals->Options.getAsBool("render.effect.tone-mapping"));
  this->Internals->Renderer->SetUseDepthPeelingPass(
    this->Internals->Options.getAsBool("render.effect.translucency-support"));

  this->Internals->Renderer->SetBackground(
    this->Internals->Options.getAsDoubleVector("render.background.color").data());
  this->Internals->Renderer->SetUseBlurBackground(
    this->Internals->Options.getAsBool("render.background.blur"));
  this->Internals->Renderer->SetBlurCircleOfConfusionRadius(
    this->Internals->Options.getAsDouble("render.background.blur.coc"));
  this->Internals->Renderer->SetHDRIFile(
    this->Internals->Options.getAsString("render.background.hdri"));
  this->Internals->Renderer->SetLightIntensity(
    this->Internals->Options.getAsDouble("render.light.intensity"));

  this->Internals->Renderer->SetFontFile(this->Internals->Options.getAsString("ui.font-file"));

  this->Internals->Renderer->SetGridUnitSquare(
    this->Internals->Options.getAsDouble("render.grid.unit"));
  this->Internals->Renderer->SetGridSubdivisions(
    this->Internals->Options.getAsInt("render.grid.subdivisions"));
  this->Internals->Renderer->SetGridAbsolute(
    this->Internals->Options.getAsBool("render.grid.absolute"));
  this->Internals->Renderer->ShowGrid(this->Internals->Options.getAsBool("render.grid.enable"));

  vtkF3DRendererWithColoring* renWithColor =
    vtkF3DRendererWithColoring::SafeDownCast(this->Internals->Renderer);

  if (renWithColor)
  {
    renWithColor->SetSurfaceColor(
      this->Internals->Options.getAsDoubleVector("model.color.rgb").data());
    renWithColor->SetOpacity(this->Internals->Options.getAsDouble("model.color.opacity"));
    renWithColor->SetTextureBaseColor(this->Internals->Options.getAsString("model.color.texture"));
    renWithColor->SetRoughness(this->Internals->Options.getAsDouble("model.material.roughness"));
    renWithColor->SetMetallic(this->Internals->Options.getAsDouble("model.material.metallic"));
    renWithColor->SetTextureMaterial(
      this->Internals->Options.getAsString("model.material.texture"));
    renWithColor->SetTextureEmissive(
      this->Internals->Options.getAsString("model.emissive.texture"));
    renWithColor->SetEmissiveFactor(
      this->Internals->Options.getAsDoubleVector("model.emissive.factor").data());
    renWithColor->SetTextureNormal(this->Internals->Options.getAsString("model.normal.texture"));
    renWithColor->SetNormalScale(this->Internals->Options.getAsDouble("model.normal.scale"));
    renWithColor->SetTextureMatCap(this->Internals->Options.getAsString("model.matcap.texture"));

    renWithColor->SetColoring(this->Internals->Options.getAsBool("model.scivis.cells"),
      this->Internals->Options.getAsString("model.scivis.array-name"),
      this->Internals->Options.getAsInt("model.scivis.component"));
    renWithColor->SetScalarBarRange(
      this->Internals->Options.getAsDoubleVector("model.scivis.range"));
    renWithColor->SetColormap(this->Internals->Options.getAsDoubleVector("model.scivis.colormap"));
    renWithColor->ShowScalarBar(this->Internals->Options.getAsBool("ui.bar"));

    renWithColor->SetUsePointSprites(
      this->Internals->Options.getAsBool("model.point-sprites.enable"));
    renWithColor->SetUseVolume(this->Internals->Options.getAsBool("model.volume.enable"));
    renWithColor->SetUseInverseOpacityFunction(
      this->Internals->Options.getAsBool("model.volume.inverse"));
  }

  this->Internals->Renderer->UpdateActors();
}

//----------------------------------------------------------------------------
void window_impl::PrintSceneDescription(log::VerboseLevel level)
{
  log::print(level, this->Internals->Renderer->GetSceneDescription());
}

//----------------------------------------------------------------------------
void window_impl::PrintColoringDescription(log::VerboseLevel level)
{
  vtkF3DRendererWithColoring* renWithColor =
    vtkF3DRendererWithColoring::SafeDownCast(this->Internals->Renderer);
  if (renWithColor)
  {
    std::string descr = renWithColor->GetColoringDescription();
    if (!descr.empty())
    {
      log::print(level, descr);
    }
  }
}

//----------------------------------------------------------------------------
vtkRenderWindow* window_impl::GetRenderWindow()
{
  return this->Internals->RenWin;
}

//----------------------------------------------------------------------------
bool window_impl::render()
{
  this->UpdateDynamicOptions();
  this->Internals->RenWin->Render();
  return true;
}

//----------------------------------------------------------------------------
image window_impl::renderToImage(bool noBackground)
{
  this->UpdateDynamicOptions();

  vtkNew<vtkWindowToImageFilter> rtW2if;
  rtW2if->SetInput(this->Internals->RenWin);

  if (noBackground)
  {
    // we need to set the background to black to avoid blending issues with translucent
    // objects when saving to file with no background
    this->Internals->RenWin->GetRenderers()->GetFirstRenderer()->SetBackground(0, 0, 0);
    rtW2if->SetInputBufferTypeToRGBA();
  }

  vtkNew<vtkImageExport> exporter;
  exporter->SetInputConnection(rtW2if->GetOutputPort());
  exporter->ImageLowerLeftOn();

  int* dims = exporter->GetDataDimensions();
  int cmp = exporter->GetDataNumberOfScalarComponents();

  image output;
  output.setResolution(dims[0], dims[1]);
  output.setChannelCount(cmp);

  exporter->Export(output.getData());

  return output;
}

//----------------------------------------------------------------------------
void window_impl::SetImporterForColoring(vtkF3DGenericImporter* importer)
{
  vtkF3DRendererWithColoring* renWithColor =
    vtkF3DRendererWithColoring::SafeDownCast(this->Internals->Renderer);
  if (renWithColor)
  {
    renWithColor->SetImporter(importer);
  }
}

//----------------------------------------------------------------------------
void window_impl::SetCachePath(const std::string& cachePath)
{
  this->Internals->CachePath = cachePath;
}
};
