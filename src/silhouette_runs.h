#pragma once
#include <vector>
#include <cstdint>
namespace silhouette {
struct RasterRect { int x,y,w,h,kind; };
// Merge identical runs on consecutive rows. Every labelled pixel is covered
// exactly once, so alpha blending and the detailed contour are unchanged.
inline size_t MergeRuns(const std::vector<uint8_t>& labels,int width,int height,std::vector<RasterRect>& out) {
    out.clear();std::vector<size_t> previous,current;size_t runs=0;
    for(int y=0;y<height;++y){current.clear();size_t cursor=0;
        for(int x=0;x<width;){int start=x,k=labels[y*width+x++];while(x<width&&labels[y*width+x]==k)++x;if(!k)continue;++runs;
            while(cursor<previous.size()&&out[previous[cursor]].x<start)++cursor;
            size_t id=out.size();
            if(cursor<previous.size()){auto candidate=previous[cursor];auto& r=out[candidate];if(r.x==start&&r.w==x-start&&r.kind==k){++r.h;id=candidate;}}
            if(id==out.size())out.push_back({start,y,x-start,1,k});current.push_back(id);
        }previous.swap(current);
    }return runs;
}
}
