// Modern self-drawn panel: rounded card, real font, toggles, sliders.
// Drawn with the legacy GL that X-Plane exposes to plugins; text comes from a
// GDI glyph atlas so it stays sharp at any size.
#pragma once

bool Panel_Init();       // creates the GL window (returns false on failure)
void Panel_Shutdown();
void Panel_Show(bool show);
void Panel_Toggle();
bool Panel_Visible();
bool Panel_Active();
void Panel_Tick();
void Panel_Rebuild();    // re-create fonts after a UI scale change
void Panel_CycleScale();
