///|/ Copyright (c) Prusa Research 2018 - 2023 Vojtěch Bubník @bubnikv, Enrico Turri @enricoturri1966, Oleksandra Iushchenko @YuSanka, Filip Sykala @Jony01, Lukáš Matěna @lukasmatena, David Kocík @kocikdav, Vojtěch Král @vojtechkral
///|/ Copyright (c) BambuStudio 2023 manch1n @manch1n
///|/ Copyright (c) 2019 BeldrothTheGold @BeldrothTheGold
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/
#ifndef _prusaslicer_technologies_h_
#define _prusaslicer_technologies_h_

//=============
// debug techs
//=============
// Shows camera target in the 3D scene
#define ENABLE_SHOW_CAMERA_TARGET 0
// Log debug messages to console when changing selection
#define ENABLE_SELECTION_DEBUG_OUTPUT 0
// Renders a small sphere in the center of the bounding box of the current selection when no gizmo is active
#define ENABLE_RENDER_SELECTION_CENTER 0
// Shows an imgui dialog with camera related data
#define ENABLE_CAMERA_STATISTICS 0
// Disable synchronization of unselected instances
#define DISABLE_INSTANCES_SYNCH 0
// Use wxDataViewRender instead of wxDataViewCustomRenderer
#define ENABLE_NONCUSTOM_DATA_VIEW_RENDERING 0
// Enable project dirty state manager debug window
#define ENABLE_PROJECT_DIRTY_STATE_DEBUG_WINDOW 0
// Enable Measure Gizmo debug window
#define ENABLE_MEASURE_GIZMO_DEBUG 0
// Enable scene raycast picking debug window
#define ENABLE_RAYCAST_PICKING_DEBUG 0
// Shows an imgui dialog with GLModel statistics data
#define ENABLE_GLMODEL_STATISTICS 0
// Shows an imgui dialog containing the matrices of the selected volumes
#define ENABLE_MATRICES_DEBUG 0
// Shows an imgui dialog containing data from class ObjectManipulation
#define ENABLE_OBJECT_MANIPULATION_DEBUG 0
// Shows an imgui dialog containing data for class GLCanvas3D::SLAView
#define ENABLE_SLA_VIEW_DEBUG_WINDOW 0

// Enable rendering of objects using environment map
#define ENABLE_ENVIRONMENT_MAP 0
// Enable smoothing of objects normals
#define ENABLE_SMOOTH_NORMALS 0

#endif // _prusaslicer_technologies_h_
