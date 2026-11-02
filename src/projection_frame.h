#pragma once
#include <cmath>
namespace ui {
struct ProjectionFrame {
    float view[16]{},projection[16]{},viewport[4]{};
    bool Project(const float world[3],float& x,float& y,float& depth) const {
        depth=world[0]*view[2]+world[1]*view[6]+world[2]*view[10]+view[14];
        if(!std::isfinite(depth)||depth<=.01f)return false;
        float inverse=1.f/depth;
        float vx=inverse*(world[0]*view[0]+world[1]*view[4]+world[2]*view[8]+view[12]);
        float vy=inverse*(world[0]*view[1]+world[1]*view[5]+world[2]*view[9]+view[13]);
        x=((vy*projection[4]+vx*projection[0]+inverse*depth*projection[8]+projection[12])*.5f+.5f)*viewport[2]+viewport[0];
        y=(.5f-(vy*projection[5]+vx*projection[1]+inverse*depth*projection[9]+projection[13])*.5f)*viewport[3]+viewport[1];
        return std::isfinite(x)&&std::isfinite(y);
    }
};
bool CaptureProjection(ProjectionFrame& frame);
}
