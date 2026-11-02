#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace silhouette {
struct SurfacePixel { float inverseDepth=0; std::array<float,3> normal{}; unsigned char shade=0; };
// The depth buffer belongs only to the chest. World walls never participate.
inline void RasterSurface(std::vector<SurfacePixel>& buffer,int width,int height,
    const std::array<float,3> p[3],const std::array<float,3>& normal,unsigned char shade) {
    float area=(p[1][1]-p[2][1])*(p[0][0]-p[2][0])+(p[2][0]-p[1][0])*(p[0][1]-p[2][1]);
    if(!std::isfinite(area)||std::fabs(area)<1e-6f)return;
    int x0=std::max(0,int(std::floor(std::min({p[0][0],p[1][0],p[2][0]}))));
    int x1=std::min(width-1,int(std::ceil(std::max({p[0][0],p[1][0],p[2][0]}))));
    int y0=std::max(0,int(std::floor(std::min({p[0][1],p[1][1],p[2][1]}))));
    int y1=std::min(height-1,int(std::ceil(std::max({p[0][1],p[1][1],p[2][1]}))));
    // Scan only the triangle's covered span, instead of its entire bounding box.
    const float ax=(p[1][1]-p[2][1])/area,bx=(p[2][1]-p[0][1])/area;
    const float izStep=ax*(p[0][2]-p[2][2])+bx*(p[1][2]-p[2][2]);
    for(int y=y0;y<=y1;++y){
        float scan=y+.5f,lo=1e20f,hi=-1e20f;int hits=0;
        for(int j=0;j<3;++j){const auto& a=p[j];const auto& b=p[(j+1)%3];
            if((a[1]<=scan&&b[1]>scan)||(b[1]<=scan&&a[1]>scan)){
                float x=a[0]+(scan-a[1])*(b[0]-a[0])/(b[1]-a[1]);lo=std::min(lo,x);hi=std::max(hi,x);++hits;
            }
        }
        if(hits!=2)continue;
        int left=std::max(x0,int(std::ceil(lo-.5f))),right=std::min(x1,int(std::floor(hi-.5f)));
        float a=((p[1][1]-p[2][1])*(left+.5f-p[2][0])+(p[2][0]-p[1][0])*(scan-p[2][1]))/area;
        float b=((p[2][1]-p[0][1])*(left+.5f-p[2][0])+(p[0][0]-p[2][0])*(scan-p[2][1]))/area;
        float iz=a*p[0][2]+b*p[1][2]+(1-a-b)*p[2][2];
        for(int x=left;x<=right;++x,iz+=izStep){auto& dest=buffer[y*width+x];if(iz>dest.inverseDepth)dest={iz,normal,shade};}
    }
}
inline bool SurfaceCrease(const SurfacePixel& a,const SurfacePixel& b) {
    if(a.inverseDepth<=0||b.inverseDepth<=0)return false;
    float dot=0;for(int j=0;j<3;++j)dot+=a.normal[j]*b.normal[j];
    return std::fabs(dot)<.82f;
}
}
