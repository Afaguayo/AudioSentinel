// Cairo/Pango drawing for the Linux dashboard and tray icon. Needs no
// display, so it also renders screenshots in headless CI.
#pragma once

#include "../core/dashboard.h"

#include <cairo.h>

#include <string>

// Logical size of the dashboard, in pixels at 1x.
constexpr int DASHBOARD_WIDTH = 440;
constexpr int DASHBOARD_HEIGHT = 450;

void DrawDashboard(cairo_t* cr, double width, double height, const as::DashboardModel& m);
void DrawTrayIcon(cairo_t* cr, double size, as::Zone zone, double fraction);

bool RenderDashboardPng(const std::string& path, const as::DashboardModel& m, double scale);
bool RenderTrayIconPng(const std::string& path, int size, as::Zone zone, double fraction);
