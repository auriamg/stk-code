//
//  SuperTuxKart - a fun racing game with go-kart
//  Copyright (C) 2009 Joerg Henrichs
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 3
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

#include "graphics/irr_driver.hpp"

#include "config/user_config.hpp"
#include "graphics/callbacks.hpp"
#include "graphics/camera.hpp"
#include "graphics/glow.hpp"
#include "graphics/glwrap.hpp"
#include "graphics/lens_flare.hpp"
#include "graphics/light.hpp"
#include "graphics/lod_node.hpp"
#include "graphics/material_manager.hpp"
#include "graphics/particle_kind_manager.hpp"
#include "graphics/per_camera_node.hpp"
#include "graphics/post_processing.hpp"
#include "graphics/referee.hpp"
#include "graphics/rtts.hpp"
#include "graphics/screenquad.hpp"
#include "graphics/shaders.hpp"
#include "graphics/shadow_importance.hpp"
#include "graphics/wind.hpp"
#include "io/file_manager.hpp"
#include "items/item.hpp"
#include "items/item_manager.hpp"
#include "modes/world.hpp"
#include "physics/physics.hpp"
#include "tracks/track.hpp"
#include "utils/constants.hpp"
#include "utils/helpers.hpp"
#include "utils/log.hpp"
#include "utils/profiler.hpp"

#include <algorithm>

void IrrDriver::renderGLSL(float dt)
{
    World *world = World::getWorld(); // Never NULL.

    // Overrides
    video::SOverrideMaterial &overridemat = m_video_driver->getOverrideMaterial();
    overridemat.EnablePasses = scene::ESNRP_SOLID | scene::ESNRP_TRANSPARENT;
    overridemat.EnableFlags = 0;

    if (m_wireframe)
    {
        overridemat.Material.Wireframe = 1;
        overridemat.EnableFlags |= video::EMF_WIREFRAME;
    }
    if (m_mipviz)
    {
        overridemat.Material.MaterialType = m_shaders->getShader(ES_MIPVIZ);
        overridemat.EnableFlags |= video::EMF_MATERIAL_TYPE;
        overridemat.EnablePasses = scene::ESNRP_SOLID;
    }

    // Get a list of all glowing things. The driver's list contains the static ones,
    // here we add items, as they may disappear each frame.
    std::vector<GlowData> glows = m_glowing;
    std::vector<GlowNode *> transparent_glow_nodes;

    ItemManager * const items = ItemManager::get();
    const u32 itemcount = items->getNumberOfItems();
    u32 i;

    // For each static node, give it a glow representation
    const u32 staticglows = glows.size();
    for (i = 0; i < staticglows; i++)
    {
        scene::ISceneNode * const node = glows[i].node;

        const float radius = (node->getBoundingBox().getExtent().getLength() / 2) * 2.0f;
        GlowNode * const repnode = new GlowNode(irr_driver->getSceneManager(), radius);
        repnode->setPosition(node->getTransformedBoundingBox().getCenter());
        transparent_glow_nodes.push_back(repnode);
    }

    for (i = 0; i < itemcount; i++)
    {
        Item * const item = items->getItem(i);
        if (!item) continue;
        const Item::ItemType type = item->getType();

        if (type != Item::ITEM_NITRO_BIG && type != Item::ITEM_NITRO_SMALL &&
            type != Item::ITEM_BONUS_BOX && type != Item::ITEM_BANANA && type != Item::ITEM_BUBBLEGUM)
            continue;

        LODNode * const lod = (LODNode *) item->getSceneNode();
        if (!lod->isVisible()) continue;

        const int level = lod->getLevel();
        if (level < 0) continue;

        scene::ISceneNode * const node = lod->getAllNodes()[level];
        node->updateAbsolutePosition();

        GlowData dat;
        dat.node = node;

        dat.r = 1.0f;
        dat.g = 1.0f;
        dat.b = 1.0f;

        const video::SColorf &c = ItemManager::getGlowColor(type);
        dat.r = c.getRed();
        dat.g = c.getGreen();
        dat.b = c.getBlue();

        glows.push_back(dat);

        // Push back its representation too
        const float radius = (node->getBoundingBox().getExtent().getLength() / 2) * 2.0f;
        GlowNode * const repnode = new GlowNode(irr_driver->getSceneManager(), radius);
        repnode->setPosition(node->getTransformedBoundingBox().getCenter());
        transparent_glow_nodes.push_back(repnode);
    }

    // Start the RTT for post-processing.
    // We do this before beginScene() because we want to capture the glClear()
    // because of tracks that do not have skyboxes (generally add-on tracks)
    m_post_processing->begin();
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, false);

    m_video_driver->beginScene(/*backBuffer clear*/ true, /*zBuffer*/ true,
                               world->getClearColor());

    // Clear normal and depth to zero
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_NORMAL_AND_DEPTH), true, false, video::SColor(0,0,0,0));
    // Clear specular map to zero
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_SPECULARMAP), true, false, video::SColor(0,0,0,0));

    irr_driver->getVideoDriver()->enableMaterial2D();
    RaceGUIBase *rg = world->getRaceGUI();
    if (rg) rg->update(dt);


    for(unsigned int cam = 0; cam < Camera::getNumCameras(); cam++)
    {
        Camera * const camera = Camera::getCamera(cam);
        scene::ICameraSceneNode * const camnode = camera->getCameraSceneNode();

#ifdef ENABLE_PROFILER
        std::ostringstream oss;
        oss << "drawAll() for kart " << cam << std::flush;
        PROFILER_PUSH_CPU_MARKER(oss.str().c_str(), (cam+1)*60,
                                 0x00, 0x00);
#endif
        camera->activate();
        rg->preRenderCallback(camera);   // adjusts start referee

        const u32 bgnodes = m_background.size();
/*        if (bgnodes)
        {
            // If there are background nodes (3d skybox), draw them now.
            m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, false);

            m_renderpass = scene::ESNRP_SKY_BOX;
            m_scene_manager->drawAll(m_renderpass);

            const video::SOverrideMaterial prev = overridemat;
            overridemat.Enabled = 1;
            overridemat.EnableFlags = video::EMF_MATERIAL_TYPE;
            overridemat.Material.MaterialType = video::EMT_TRANSPARENT_ALPHA_CHANNEL_REF;

            for (i = 0; i < bgnodes; i++)
            {
                m_background[i]->setPosition(camnode->getPosition() * 0.97f);
                m_background[i]->updateAbsolutePosition();
                m_background[i]->render();
            }

            overridemat = prev;
            m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, true);
        }*/

        // Fire up the MRT
		irr_driver->getVideoDriver()->setRenderTarget(irr_driver->getRTT(RTT_NORMAL_AND_DEPTH), false, false);
		PROFILER_PUSH_CPU_MARKER("- Solid Pass 1", 0xFF, 0x00, 0x00);
        m_renderpass = scene::ESNRP_CAMERA | scene::ESNRP_SOLID;
		glDepthFunc(GL_LEQUAL);
		glEnable(GL_DEPTH_TEST);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
        irr_driver->setPhase(SOLID_NORMAL_AND_DEPTH_PASS);
        m_scene_manager->drawAll(m_renderpass);
        irr_driver->setProjMatrix(irr_driver->getVideoDriver()->getTransform(video::ETS_PROJECTION));
        irr_driver->setViewMatrix(irr_driver->getVideoDriver()->getTransform(video::ETS_VIEW));
        irr_driver->genProjViewMatrix();

		PROFILER_POP_CPU_MARKER();

        // Todo : reenable glow and shadows
        //ShadowImportanceProvider * const sicb = (ShadowImportanceProvider *)
        //                                         irr_driver->getCallback(ES_SHADOW_IMPORTANCE);
        //sicb->updateIPVMatrix();

        // Used to cull glowing items & lights
        const core::aabbox3df cambox = camnode->getViewFrustum()->getBoundingBox();

        PROFILER_PUSH_CPU_MARKER("- Shadow", 0x30, 0x6F, 0x90);
        // Shadows
        if (!m_mipviz && !m_wireframe && UserConfigParams::m_shadows)
           //&& World::getWorld()->getTrack()->hasShadows())
        {
            renderShadows(camnode, camera);
        }
        PROFILER_POP_CPU_MARKER();

        PROFILER_PUSH_CPU_MARKER("- Light", 0x00, 0xFF, 0x00);

        // Lights
        renderLights(cambox, camnode, overridemat, cam, dt);
		PROFILER_POP_CPU_MARKER();

		PROFILER_PUSH_CPU_MARKER("- Solid Pass 2", 0x00, 0x00, 0xFF);
        irr_driver->setPhase(SOLID_LIT_PASS);
		glEnable(GL_DEPTH_TEST);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_FALSE);
		glDisable(GL_BLEND);
        m_renderpass = scene::ESNRP_CAMERA | scene::ESNRP_SOLID;
        m_scene_manager->drawAll(m_renderpass);

        PROFILER_POP_CPU_MARKER();

		  if (World::getWorld()->getTrack()->isFogEnabled())
		  {
			  PROFILER_PUSH_CPU_MARKER("- Fog", 0xFF, 0x00, 0x00);
			  m_post_processing->renderFog(irr_driver->getInvProjMatrix());
			  PROFILER_POP_CPU_MARKER();
		  }

        PROFILER_PUSH_CPU_MARKER("- Glow", 0xFF, 0xFF, 0x00);

		// Render anything glowing.
		if (!m_mipviz && !m_wireframe)
		{
			irr_driver->setPhase(GLOW_PASS);
			renderGlow(overridemat, glows, cambox, cam);
		} // end glow

        PROFILER_POP_CPU_MARKER();

		if (!SkyboxTextures.empty())
		{
			PROFILER_PUSH_CPU_MARKER("- Skybox", 0xFF, 0x00, 0xFF);
			renderSkybox();
			PROFILER_POP_CPU_MARKER();
		}

        PROFILER_PUSH_CPU_MARKER("- Lensflare/godray", 0x00, 0xFF, 0xFF);
        // Is the lens flare enabled & visible? Check last frame's query.
        const bool hasflare = World::getWorld()->getTrack()->hasLensFlare();
        const bool hasgodrays = World::getWorld()->getTrack()->hasGodRays();
        if (hasflare || hasgodrays)
        {
            irr::video::COpenGLDriver*	gl_driver = (irr::video::COpenGLDriver*)m_device->getVideoDriver();

            GLuint res;
            gl_driver->extGlGetQueryObjectuiv(m_lensflare_query, GL_QUERY_RESULT, &res);
            m_post_processing->setSunPixels(res);

            // Prepare the query for the next frame.
            gl_driver->extGlBeginQuery(GL_SAMPLES_PASSED_ARB, m_lensflare_query);
            m_scene_manager->setCurrentRendertime(scene::ESNRP_SOLID);
            m_scene_manager->drawAll(scene::ESNRP_CAMERA);
            m_sun_interposer->render();
            gl_driver->extGlEndQuery(GL_SAMPLES_PASSED_ARB);

            m_lensflare->setStrength(res / 4000.0f);

            if (hasflare)
                m_lensflare->OnRegisterSceneNode();

            // Make sure the color mask is reset
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        PROFILER_POP_CPU_MARKER();

        // We need to re-render camera due to the per-cam-node hack.
        PROFILER_PUSH_CPU_MARKER("- Transparent Pass", 0xFF, 0x00, 0x00);
		irr_driver->setPhase(TRANSPARENT_PASS);
		m_renderpass = scene::ESNRP_CAMERA | scene::ESNRP_TRANSPARENT;
		glEnable(GL_DEPTH_TEST);
		glDisable(GL_ALPHA_TEST);
		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_CULL_FACE);
        m_scene_manager->drawAll(m_renderpass);
		PROFILER_POP_CPU_MARKER();

		PROFILER_PUSH_CPU_MARKER("- Particles", 0xFF, 0xFF, 0x00);
		m_renderpass = scene::ESNRP_CAMERA | scene::ESNRP_TRANSPARENT_EFFECT;
		glDepthMask(GL_FALSE);
		glDisable(GL_CULL_FACE);
		glEnable(GL_BLEND);
		glBlendEquation(GL_FUNC_ADD);
		m_scene_manager->drawAll(m_renderpass);
		PROFILER_POP_CPU_MARKER();

        PROFILER_PUSH_CPU_MARKER("- Displacement", 0x00, 0x00, 0xFF);
        // Handle displacing nodes, if any
        const u32 displacingcount = m_displacing.size();
        if (displacingcount)
        {
            renderDisplacement(overridemat, cam);
        }
        PROFILER_POP_CPU_MARKER();

        // Drawing for this cam done, cleanup
        const u32 glowrepcount = transparent_glow_nodes.size();
        for (i = 0; i < glowrepcount; i++)
        {
            transparent_glow_nodes[i]->remove();
            transparent_glow_nodes[i]->drop();
        }

        PROFILER_POP_CPU_MARKER();

        // Note that drawAll must be called before rendering
        // the bullet debug view, since otherwise the camera
        // is not set up properly. This is only used for
        // the bullet debug view.
        if (UserConfigParams::m_artist_debug_mode)
            World::getWorld()->getPhysics()->draw();
    }   // for i<world->getNumKarts()

    PROFILER_PUSH_CPU_MARKER("Postprocessing", 0xFF, 0xFF, 0x00);
    // Render the post-processed scene
    m_post_processing->render();
    PROFILER_POP_CPU_MARKER();

    // Set the viewport back to the full screen for race gui
    m_video_driver->setViewPort(core::recti(0, 0,
                                            UserConfigParams::m_width,
                                            UserConfigParams::m_height));

    for(unsigned int i=0; i<Camera::getNumCameras(); i++)
    {
        Camera *camera = Camera::getCamera(i);
        char marker_name[100];
        sprintf(marker_name, "renderPlayerView() for kart %d", i);

        PROFILER_PUSH_CPU_MARKER(marker_name, 0x00, 0x00, (i+1)*60);
        rg->renderPlayerView(camera, dt);

        PROFILER_POP_CPU_MARKER();
    }  // for i<getNumKarts

    PROFILER_PUSH_CPU_MARKER("GUIEngine", 0x75, 0x75, 0x75);
    // Either render the gui, or the global elements of the race gui.
    GUIEngine::render(dt);
    PROFILER_POP_CPU_MARKER();

    // Render the profiler
    if(UserConfigParams::m_profiler_enabled)
    {
        PROFILER_DRAW();
    }


#ifdef DEBUG
    drawDebugMeshes();
#endif

    PROFILER_PUSH_CPU_MARKER("EndSccene", 0x45, 0x75, 0x45);
    m_video_driver->endScene();
    PROFILER_POP_CPU_MARKER();

    getPostProcessing()->update(dt);
}

// --------------------------------------------

void IrrDriver::renderFixed(float dt)
{
    World *world = World::getWorld(); // Never NULL.

    m_video_driver->beginScene(/*backBuffer clear*/ true, /*zBuffer*/ true,
                               world->getClearColor());

    irr_driver->getVideoDriver()->enableMaterial2D();

    RaceGUIBase *rg = world->getRaceGUI();
    if (rg) rg->update(dt);


    for(unsigned int i=0; i<Camera::getNumCameras(); i++)
    {
        Camera *camera = Camera::getCamera(i);

#ifdef ENABLE_PROFILER
        std::ostringstream oss;
        oss << "drawAll() for kart " << i << std::flush;
        PROFILER_PUSH_CPU_MARKER(oss.str().c_str(), (i+1)*60,
                                 0x00, 0x00);
#endif
        camera->activate();
        rg->preRenderCallback(camera);   // adjusts start referee

        m_renderpass = ~0;
        m_scene_manager->drawAll();

        PROFILER_POP_CPU_MARKER();

        // Note that drawAll must be called before rendering
        // the bullet debug view, since otherwise the camera
        // is not set up properly. This is only used for
        // the bullet debug view.
        if (UserConfigParams::m_artist_debug_mode)
            World::getWorld()->getPhysics()->draw();
    }   // for i<world->getNumKarts()

    // Set the viewport back to the full screen for race gui
    m_video_driver->setViewPort(core::recti(0, 0,
                                            UserConfigParams::m_width,
                                            UserConfigParams::m_height));

    for(unsigned int i=0; i<Camera::getNumCameras(); i++)
    {
        Camera *camera = Camera::getCamera(i);
        char marker_name[100];
        sprintf(marker_name, "renderPlayerView() for kart %d", i);

        PROFILER_PUSH_CPU_MARKER(marker_name, 0x00, 0x00, (i+1)*60);
        rg->renderPlayerView(camera, dt);
        PROFILER_POP_CPU_MARKER();

    }  // for i<getNumKarts

    // Either render the gui, or the global elements of the race gui.
    GUIEngine::render(dt);

    // Render the profiler
    if(UserConfigParams::m_profiler_enabled)
    {
        PROFILER_DRAW();
    }

#ifdef DEBUG
    drawDebugMeshes();
#endif

    m_video_driver->endScene();
}

// ----------------------------------------------------------------------------

void IrrDriver::renderShadows(//ShadowImportanceProvider * const sicb,
                              scene::ICameraSceneNode * const camnode,
                              //video::SOverrideMaterial &overridemat,
                              Camera * const camera)
{
    m_scene_manager->setCurrentRendertime(scene::ESNRP_SOLID);

    const Vec3 *vmin, *vmax;
    World::getWorld()->getTrack()->getAABB(&vmin, &vmax);

    const float oldfar = camnode->getFarValue();
    const float oldnear = camnode->getNearValue();
    float FarValues[] =
    {
        20.,
        50.,
        100.,
        oldfar,
    };
    float NearValues[] =
    {
        oldnear,
        20.,
        50.,
        100.,
    };

    const core::matrix4 &SunCamViewMatrix = m_suncam->getViewMatrix();
    sun_ortho_matrix.clear();

    // Build the 3 ortho projection (for the 3 shadow resolution levels)
    for (unsigned i = 0; i < 4; i++)
    {
        camnode->setFarValue(FarValues[i]);
        camnode->setNearValue(NearValues[i]);
        camnode->render();
        const core::aabbox3df smallcambox = camnode->
            getViewFrustum()->getBoundingBox();
        core::aabbox3df trackbox(vmin->toIrrVector(), vmax->toIrrVector() -
            core::vector3df(0, 30, 0));


        // Set up a nice ortho projection that contains our camera frustum
        core::aabbox3df box = smallcambox;
        box = box.intersect(trackbox);


        SunCamViewMatrix.transformBoxEx(trackbox);
        SunCamViewMatrix.transformBoxEx(box);

        core::vector3df extent = trackbox.getExtent();
        const float w = fabsf(extent.X);
        const float h = fabsf(extent.Y);
        float z = box.MaxEdge.Z;

        // Snap to texels
        const float units_per_w = w / 1024;
        const float units_per_h = h / 1024;

        float left = box.MinEdge.X;
        float right = box.MaxEdge.X;
        float up = box.MaxEdge.Y;
        float down = box.MinEdge.Y;

        left -= fmodf(left, units_per_w);
        right -= fmodf(right, units_per_w);
        up -= fmodf(up, units_per_h);
        down -= fmodf(down, units_per_h);
        z -= fmodf(z, 0.5f);

        // FIXME: quick and dirt (and wrong) workaround to avoid division by zero
        if (left == right) right += 0.1f;
        if (up == down) down += 0.1f;
        if (z == 30) z += 0.1f;

        core::matrix4 tmp_matrix;

        tmp_matrix.buildProjectionMatrixOrthoLH(left, right,
            up, down,
            30, z);
        m_suncam->setProjectionMatrix(tmp_matrix, true);
        m_scene_manager->setActiveCamera(m_suncam);
        m_suncam->render();

        sun_ortho_matrix.push_back(getVideoDriver()->getTransform(video::ETS_PROJECTION) * getVideoDriver()->getTransform(video::ETS_VIEW));
    }
    assert(sun_ortho_matrix.size() == 4);

    irr_driver->setPhase(SHADOW_PASS);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_FRONT);
    glBindFramebuffer(GL_FRAMEBUFFER, m_rtts->getShadowFBO());
    glViewport(0, 0, 1024, 1024);
    glClear(GL_DEPTH_BUFFER_BIT);
    glDrawBuffer(GL_NONE);
    m_scene_manager->drawAll(scene::ESNRP_SOLID);
    glCullFace(GL_BACK);

    camnode->setNearValue(oldnear);
    camnode->setFarValue(oldfar);
    camnode->render();
    camera->activate();
    m_scene_manager->drawAll(scene::ESNRP_CAMERA);


    //sun_ortho_matrix *= m_suncam->getViewMatrix();
/*    ((SunLightProvider *) m_shaders->m_callbacks[ES_SUNLIGHT])->setShadowMatrix(ortho);
    sicb->setShadowMatrix(ortho);

    overridemat.Enabled = 0;

    // Render the importance map
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLLAPSE), true, true);

    m_shadow_importance->render();

    CollapseProvider * const colcb = (CollapseProvider *)
                                            m_shaders->
                                            m_callbacks[ES_COLLAPSE];
    ScreenQuad sq(m_video_driver);
    sq.setMaterialType(m_shaders->getShader(ES_COLLAPSE));
    sq.setTexture(m_rtts->getRTT(RTT_COLLAPSE));
    sq.getMaterial().setFlag(EMF_BILINEAR_FILTER, false);

    const TypeRTT oldh = tick ? RTT_COLLAPSEH : RTT_COLLAPSEH2;
    const TypeRTT oldv = tick ? RTT_COLLAPSEV : RTT_COLLAPSEV2;
    const TypeRTT curh = tick ? RTT_COLLAPSEH2 : RTT_COLLAPSEH;
    const TypeRTT curv = tick ? RTT_COLLAPSEV2 : RTT_COLLAPSEV;

    colcb->setResolution(1, m_rtts->getRTT(RTT_WARPV)->getSize().Height);
    sq.setTexture(m_rtts->getRTT(oldh), 1);
    sq.render(m_rtts->getRTT(RTT_WARPH));

    colcb->setResolution(m_rtts->getRTT(RTT_WARPV)->getSize().Height, 1);
    sq.setTexture(m_rtts->getRTT(oldv), 1);
    sq.render(m_rtts->getRTT(RTT_WARPV));

    sq.setTexture(0, 1);
    ((GaussianBlurProvider *) m_shaders->m_callbacks[ES_GAUSSIAN3H])->setResolution(
                m_rtts->getRTT(RTT_WARPV)->getSize().Height,
                m_rtts->getRTT(RTT_WARPV)->getSize().Height);

    sq.setMaterialType(m_shaders->getShader(ES_GAUSSIAN6H));
    sq.setTexture(m_rtts->getRTT(RTT_WARPH));
    sq.render(m_rtts->getRTT(curh));

    sq.setMaterialType(m_shaders->getShader(ES_GAUSSIAN6V));
    sq.setTexture(m_rtts->getRTT(RTT_WARPV));
    sq.render(m_rtts->getRTT(curv));*/

    // Convert importance maps to warp maps
    //
    // It should be noted that while they do repeated work
    // calculating the min, max, and total, it's several hundred us
    // faster to do that than to do it once in a separate shader
    // (shader switch overhead, measured).
    /*colcb->setResolution(m_rtts->getRTT(RTT_WARPV)->getSize().Height,
                            m_rtts->getRTT(RTT_WARPV)->getSize().Height);

    sq.setMaterialType(m_shaders->getShader(ES_SHADOW_WARPH));
    sq.setTexture(m_rtts->getRTT(curh));
    sq.render(m_rtts->getRTT(RTT_WARPH));

    sq.setMaterialType(m_shaders->getShader(ES_SHADOW_WARPV));
    sq.setTexture(m_rtts->getRTT(curv));
    sq.render(m_rtts->getRTT(RTT_WARPV));*/

    // Actual shadow map


/*    overridemat.Material.MaterialType = m_shaders->getShader(ES_SHADOWPASS);
    overridemat.EnableFlags = video::EMF_MATERIAL_TYPE | video::EMF_TEXTURE1 |
                                video::EMF_TEXTURE2;
    overridemat.EnablePasses = scene::ESNRP_SOLID;
    overridemat.Material.setTexture(1, m_rtts->getRTT(RTT_WARPH));
    overridemat.Material.setTexture(2, m_rtts->getRTT(RTT_WARPV));
    overridemat.Material.TextureLayer[1].TextureWrapU =
    overridemat.Material.TextureLayer[1].TextureWrapV =
    overridemat.Material.TextureLayer[2].TextureWrapU =
    overridemat.Material.TextureLayer[2].TextureWrapV = video::ETC_CLAMP_TO_EDGE;
    overridemat.Material.TextureLayer[1].BilinearFilter =
    overridemat.Material.TextureLayer[2].BilinearFilter = true;
    overridemat.Material.TextureLayer[1].TrilinearFilter =
    overridemat.Material.TextureLayer[2].TrilinearFilter = false;
    overridemat.Material.TextureLayer[1].AnisotropicFilter =
    overridemat.Material.TextureLayer[2].AnisotropicFilter = 0;
    overridemat.Material.Wireframe = 1;
    overridemat.Enabled = true;*/



//    overridemat.EnablePasses = 0;
//    overridemat.Enabled = false;
}

// ----------------------------------------------------------------------------

void IrrDriver::renderGlow(video::SOverrideMaterial &overridemat,
                           std::vector<GlowData>& glows,
                           const core::aabbox3df& cambox,
                           int cam)
{
    m_scene_manager->setCurrentRendertime(scene::ESNRP_SOLID);

    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_TMP1), false, false);
    glClearColor(0, 0, 0, 0);
    glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    const u32 glowcount = glows.size();
    ColorizeProvider * const cb = (ColorizeProvider *) m_shaders->m_callbacks[ES_COLORIZE];

    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glStencilFunc(GL_ALWAYS, 1, ~0);
    glEnable(GL_STENCIL_TEST);

	glEnable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);

    for (u32 i = 0; i < glowcount; i++)
    {
        const GlowData &dat = glows[i];
        scene::ISceneNode * const cur = dat.node;

        // Quick box-based culling
        const core::aabbox3df nodebox = cur->getTransformedBoundingBox();
        if (!nodebox.intersectsWithBox(cambox))
            continue;

        cb->setColor(dat.r, dat.g, dat.b);
        cur->render();
    }

    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glDisable(GL_STENCIL_TEST);

    // Cool, now we have the colors set up. Progressively minify.
    video::SMaterial minimat;
    minimat.Lighting = false;
    minimat.ZWriteEnable = false;
    minimat.ZBuffer = video::ECFN_ALWAYS;
    minimat.setFlag(video::EMF_TRILINEAR_FILTER, true);

    minimat.TextureLayer[0].TextureWrapU =
    minimat.TextureLayer[0].TextureWrapV = video::ETC_CLAMP_TO_EDGE;

    // To half
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_HALF1), false, false);
	m_post_processing->renderPassThrough(m_rtts->getRTT(RTT_TMP1));

    // To quarter
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_QUARTER1), false, false);
	m_post_processing->renderPassThrough(m_rtts->getRTT(RTT_HALF1));

    // Blur it
	m_post_processing->renderGaussian6Blur(m_rtts->getRTT(RTT_QUARTER1), m_rtts->getRTT(RTT_QUARTER2), 4.f / UserConfigParams::m_width, 4.f / UserConfigParams::m_height);

	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glStencilFunc(GL_EQUAL, 0, ~0);
	glEnable(GL_STENCIL_TEST);
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, false);
	m_post_processing->renderGlow(m_rtts->getRTT(RTT_QUARTER1));
    glDisable(GL_STENCIL_TEST);
}

// ----------------------------------------------------------------------------
#define MAXLIGHT 16 // to be adjusted in pointlight.frag too
void IrrDriver::renderLights(const core::aabbox3df& cambox,
                             scene::ICameraSceneNode * const camnode,
                             video::SOverrideMaterial &overridemat,
                             int cam, float dt)
{
    for (unsigned i = 0; i < sun_ortho_matrix.size(); i++)
        sun_ortho_matrix[i] *= getInvViewMatrix();
    core::array<video::IRenderTarget> rtts;
    // Diffuse
    rtts.push_back(m_rtts->getRTT(RTT_TMP1));
    // Specular
    rtts.push_back(m_rtts->getRTT(RTT_TMP2));

    m_video_driver->setRenderTarget(rtts, true, false,
                                        video::SColor(0, 0, 0, 0));

    const u32 lightcount = m_lights.size();
    const core::vector3df &campos =
        irr_driver->getSceneManager()->getActiveCamera()->getAbsolutePosition();
    std::vector<float> accumulatedLightPos;
    std::vector<float> accumulatedLightColor;
    std::vector<float> accumulatedLightEnergy;
    std::vector<LightNode *> BucketedLN[15];
    for (unsigned int i = 0; i < lightcount; i++)
    {
        if (!m_lights[i]->isPointLight())
        {
          m_lights[i]->render();
          if (UserConfigParams::m_shadows)
              m_post_processing->renderShadowedSunlight(sun_ortho_matrix, m_rtts->getShadowDepthTex());
          else
              m_post_processing->renderSunlight();
          continue;
        }
        const core::vector3df &lightpos = (m_lights[i]->getAbsolutePosition() - campos);
        unsigned idx = (unsigned)(lightpos.getLength() / 10);
        if (idx > 14)
          continue;
        BucketedLN[idx].push_back(m_lights[i]);
    }

    unsigned lightnum = 0;

    for (unsigned i = 0; i < 15; i++)
    {
        for (unsigned j = 0; j < BucketedLN[i].size(); j++)
        {
            if (++lightnum > MAXLIGHT)
            {
                LightNode* light_node = BucketedLN[i].at(j);
                light_node->setEnergyMultiplier(0.0f);
            }
            else
            {
                LightNode* light_node = BucketedLN[i].at(j);

                float em = light_node->getEnergyMultiplier();
                if (em < 1.0f)
                {
                    light_node->setEnergyMultiplier(std::min(1.0f, em + dt));
                }

                const core::vector3df &pos = light_node->getAbsolutePosition();
                accumulatedLightPos.push_back(pos.X);
                accumulatedLightPos.push_back(pos.Y);
                accumulatedLightPos.push_back(pos.Z);
                accumulatedLightPos.push_back(0.);
                const core::vector3df &col = light_node->getColor();

                accumulatedLightColor.push_back(col.X);
                accumulatedLightColor.push_back(col.Y);
                accumulatedLightColor.push_back(col.Z);
                accumulatedLightColor.push_back(0.);
                accumulatedLightEnergy.push_back(light_node->getEffectiveEnergy());
            }
        }
        if (lightnum > MAXLIGHT)
        {
          irr_driver->setLastLightBucketDistance(i * 10);
          break;
        }
    }
	// Fill lights
	for (; lightnum < MAXLIGHT; lightnum++) {
		accumulatedLightPos.push_back(0.);
		accumulatedLightPos.push_back(0.);
		accumulatedLightPos.push_back(0.);
		accumulatedLightPos.push_back(0.);
		accumulatedLightColor.push_back(0.);
		accumulatedLightColor.push_back(0.);
		accumulatedLightColor.push_back(0.);
		accumulatedLightColor.push_back(0.);
		accumulatedLightEnergy.push_back(0.);
	}
	m_post_processing->renderPointlight(accumulatedLightPos, accumulatedLightColor, accumulatedLightEnergy);
    // Handle SSAO
    m_video_driver->setRenderTarget(irr_driver->getRTT(RTT_SSAO), true, false,
                         SColor(255, 255, 255, 255));

    if(UserConfigParams::m_ssao)
        m_post_processing->renderSSAO(irr_driver->getInvProjMatrix(), irr_driver->getProjMatrix());

    // Blur it to reduce noise.
    if(UserConfigParams::m_ssao == 1)
		m_post_processing->renderGaussian3Blur(irr_driver->getRTT(RTT_SSAO), irr_driver->getRTT(RTT_QUARTER4), 4.f / UserConfigParams::m_width, 4.f / UserConfigParams::m_height);
    else if (UserConfigParams::m_ssao == 2)
		m_post_processing->renderGaussian6Blur(irr_driver->getRTT(RTT_SSAO), irr_driver->getRTT(RTT_TMP4), 1.f / UserConfigParams::m_width, 1.f / UserConfigParams::m_height);

    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, false);
}


static GLuint cubevao = 0;
static GLuint cubevbo;
static GLuint cubeidx;

static void createcubevao()
{
	// From CSkyBoxSceneNode. Not optimal at all
	float corners[] = 
	{
		// top side
		1., 1., -1.,       1., 1.,
		1., 1., 1.,        0., 1.,
		-1., 1., 1.,       0., 0.,
		-1., 1., -1.,      1., 0.,

		// Bottom side
		1., -1., 1.,       0., 0.,
		1., -1., -1.,      1., 0.,
		-1., -1., -1.,     1., 1.,
		-1., -1., 1.,      0., 1.,

		// right side
		1., -1, -1,        1., 1.,
		1., -1, 1,         0., 1.,
		1., 1., 1.,        0., 0.,
		1., 1., -1.,       1., 0.,

		// left side
		-1., -1., 1.,      1., 1.,
		-1., -1., -1.,     0., 1.,
		-1., 1., -1.,      0., 0.,
		-1., 1., 1.,       1., 0.,

		// back side
		-1., -1., -1.,     1., 1.,
		1., -1, -1.,       0., 1.,
		1, 1, -1.,         0., 0.,
		-1, 1, -1.,        1., 0.,
		
		// front side
		1., -1., 1.,       1., 1.,
		-1., -1., 1.,      0., 1.,
		-1, 1., 1.,        0., 0.,
		1., 1., 1.,        1., 0.,
	};
	int indices[] = {
		0, 1, 2, 2, 3, 0,
		4, 5, 6, 6, 7, 4,
		8, 9, 10, 10, 11, 8,
		12, 13, 14, 14, 15, 12,
		16, 17, 18, 18, 19, 16,
		20, 21, 22, 22, 23, 20
	};

	glGenBuffers(1, &cubevbo);
	glGenBuffers(1, &cubeidx);
	glGenVertexArrays(1, &cubevao);

	glBindVertexArray(cubevao);
	glBindBuffer(GL_ARRAY_BUFFER, cubevbo);
	glBufferData(GL_ARRAY_BUFFER, 6 * 5 * 4 * sizeof(float), corners, GL_STATIC_DRAW);
	glEnableVertexAttribArray(MeshShader::ObjectUnlitShader::attrib_position);
	glEnableVertexAttribArray(MeshShader::ObjectUnlitShader::attrib_texcoord);
	glVertexAttribPointer(MeshShader::ObjectUnlitShader::attrib_position, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), 0);
	glVertexAttribPointer(MeshShader::ObjectUnlitShader::attrib_texcoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (GLvoid *)(3 * sizeof(float)));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, cubeidx);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6 * 6 * sizeof(int), indices, GL_STATIC_DRAW);
}

void IrrDriver::renderSkybox()
{
    scene::ICameraSceneNode *camera = m_scene_manager->getActiveCamera();
	if (!cubevao)
		createcubevao();
	glBindVertexArray(cubevao);
	glDisable(GL_CULL_FACE);
	assert(SkyboxTextures.size() == 6);
	core::matrix4 transform = irr_driver->getProjViewMatrix();
	core::matrix4 translate;
	translate.setTranslation(camera->getAbsolutePosition());

	// Draw the sky box between the near and far clip plane
	const f32 viewDistance = (camera->getNearValue() + camera->getFarValue()) * 0.5f;
	core::matrix4 scale;
	scale.setScale(core::vector3df(viewDistance, viewDistance, viewDistance));
	transform *= translate * scale;
	
	for (unsigned i = 0; i < 6; i++)
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, static_cast<irr::video::COpenGLTexture*>(SkyboxTextures[i])->getOpenGLTextureName());
		glUseProgram(MeshShader::ObjectUnlitShader::Program);
		MeshShader::ObjectUnlitShader::setUniforms(transform, 0);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, (GLvoid*) (6 * i * sizeof(int)));
	}
	glBindVertexArray(0);
}

// ----------------------------------------------------------------------------

void IrrDriver::renderDisplacement(video::SOverrideMaterial &overridemat,
                                   int cam)
{
    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_DISPLACE), false, false);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

	DisplaceProvider * const cb = (DisplaceProvider *)irr_driver->getCallback(ES_DISPLACE);
	cb->update();

    const int displacingcount = m_displacing.size();
	irr_driver->setPhase(DISPLACEMENT_PASS);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_ALPHA_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);

    for (int i = 0; i < displacingcount; i++)
    {
        m_scene_manager->setCurrentRendertime(scene::ESNRP_SOLID);
        m_displacing[i]->render();

        m_scene_manager->setCurrentRendertime(scene::ESNRP_TRANSPARENT);
        m_displacing[i]->render();
    }

    // Blur it
	m_post_processing->renderGaussian3Blur(m_rtts->getRTT(RTT_DISPLACE), m_rtts->getRTT(RTT_TMP2), 1.f / UserConfigParams::m_width, 1.f / UserConfigParams::m_height);

    m_video_driver->setRenderTarget(m_rtts->getRTT(RTT_COLOR), false, false);
}
