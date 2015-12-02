#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <exynos_drm.h>

int main(int argc, char *argv[])
{
	static struct {
		int fd;
		drmModeModeInfo *mode;
		uint32_t crtc_id;
		uint32_t connector_id;
	} drm;
	
	static struct {
		uint32_t handle;
		size_t size;
		uint32_t stride;
	} gem;
	
	int dma_fd;

	/*************** DRM *************/
	{
		drmModeRes *resources;
		drmModeConnector *connector = NULL;
		drmModeEncoder *encoder = NULL;
		int i, area;

		drm.fd = drmOpen("exynos", NULL);
		if (drm.fd < 0) {
			printf("failed to open drm device.\n");
			return -1;
		}

		resources = drmModeGetResources(drm.fd);
		if (!resources) {
			printf("drmModeGetResources failed: %s\n", strerror(errno));
			return -1;
		}

		/* find a connected connector: */
		for (i = 0; i < resources->count_connectors; i++) {
			connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
			if (connector->connection == DRM_MODE_CONNECTED) {
				/* it's connected, let's use this! */
				break;
			}
			drmModeFreeConnector(connector);
			connector = NULL;
		}
		if (!connector) {
			printf("no connected connector!\n");
			return -1;
		}

		/* find highest resolution mode: */
		for (i = 0, area = 0; i < connector->count_modes; i++) {
			drmModeModeInfo *current_mode = &connector->modes[i];
			int current_area = current_mode->hdisplay * current_mode->vdisplay;
			if (current_area > area) {
				drm.mode = current_mode;
				area = current_area;
			}
		}

		if (!drm.mode) {
			printf("could not find mode!\n");
			return -1;
		}

		/* find encoder: */
		for (i = 0; i < resources->count_encoders; i++) {
			encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
			if (encoder->encoder_id == connector->encoder_id)
				break;
			drmModeFreeEncoder(encoder);
			encoder = NULL;
		}

		if (!encoder) {
			printf("no encoder!\n");
			return -1;
		}

		drm.crtc_id = encoder->crtc_id;
		drm.connector_id = connector->connector_id;
	}

	/********************  GEM ***************** */
	
	{
		gem.size = drm.mode->hdisplay * drm.mode->vdisplay * 4/*BPP*/;
		struct drm_exynos_gem_create gem_create;
		int ret;

		memset(&gem_create, 0, sizeof(gem_create));
		gem_create.size = gem.size;
		gem_create.flags = EXYNOS_BO_NONCONTIG;

		ret = drmIoctl(drm.fd, DRM_IOCTL_EXYNOS_GEM_CREATE, &gem_create);
		if (ret) {
			fprintf(stderr, "DRM_IOCTL_EXYNOS_GEM_CREATE failed (size=%zu)\n", gem.size);
			return ret;
		}

		gem.handle = gem_create.handle;
		gem.stride = drm.mode->hdisplay * 4/*BPP*/;
		
		// Get the DMA file descriptor
		if(drmPrimeHandleToFD(drm.fd,gem.handle,DRM_CLOEXEC,&dma_fd))
		{
			printf("no dma handle!\n");
			return -1;
		}
	}


	/********************  EGL ***************** */
	{
		EGLDisplay display;
		EGLConfig config;
		EGLContext context;
		
		EGLint major, minor, num_config;

		static const EGLint egl_config_attribs[] = {
			EGL_BUFFER_SIZE,        32,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_DEPTH_SIZE,         EGL_DONT_CARE,
			EGL_STENCIL_SIZE,       EGL_DONT_CARE,
			EGL_RENDERABLE_TYPE,    EGL_OPENGL_ES2_BIT,
			EGL_SURFACE_TYPE,       EGL_WINDOW_BIT,
			EGL_NONE,
		};	
		
		static const EGLint context_attribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
		};		
		
		display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

		if (!eglInitialize(display, &major, &minor)) {
			printf("failed to initialize %d\n",eglGetError());
			return -1;
		}

		if (!eglBindAPI(EGL_OPENGL_ES_API)) {
			printf("failed to bind api EGL_OPENGL_ES_API\n");
			return -1;
		}
		
		if(!eglChooseConfig(display, egl_config_attribs, &config, 1, &num_config))
		{
			printf("failed to choose config\n");
			return -1;
		}
		
		context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attribs);
		if (context == NULL) {
			printf("failed to create context\n");
			return -1;
		}

		EGLint attrs[] = {
			EGL_WIDTH,
			drm.mode->hdisplay,
			EGL_HEIGHT,
			drm.mode->vdisplay,
			EGL_LINUX_DRM_FOURCC_EXT,
			DRM_FORMAT_XRGB8888,
			EGL_DMA_BUF_PLANE0_FD_EXT,
			dma_fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT,
			0,
			EGL_DMA_BUF_PLANE0_PITCH_EXT,
			gem.stride,
			EGL_NONE
		};

		typedef EGLImageKHR (*eglCreateImageKHRfn)(EGLDisplay dpy, EGLContext ctx, EGLenum target, EGLClientBuffer buffer, const EGLint *attrib_list);
		eglCreateImageKHRfn eglCreateImageKHR = (eglCreateImageKHRfn) (eglGetProcAddress("eglCreateImageKHR"));    
		
		EGLImage image = eglCreateImageKHR(display,
			EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT,
			0,
			attrs);
		
		if (image == EGL_NO_IMAGE_KHR) {
			printf("failed to create DMA pixmap %d\n",eglGetError());
			return -1;
		}	
	}
	return 0;
}
