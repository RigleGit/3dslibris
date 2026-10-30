#include "reader/image_view_utils.h"
#include "formats/common/fixed_layout_blit_utils.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
int main() {
  const u16 source[]={1,2,3,4,5,6};
  const std::vector<std::vector<u16>> gold={{1,2,3,4,5,6},{4,1,5,2,6,3},{6,5,4,3,2,1},{3,6,2,5,1,4}};
  for(int turn=0;turn<4;++turn) for(bool quality:{false,true}) {
    const int w=turn%2 ? 2 : 3,h=turn%2 ? 3 : 2;
    u16 out[6]={};
    fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(out,w,w,h,0,0,w,h,source,6,3,2,0,0,3,2,quality,0,0,w,h,turn);
    assert(std::vector<u16>(out,out+6)==gold[turn]);
    u16 clipped[6]={};
    fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(clipped,w,w,h,0,0,w,h,source,6,3,2,0,0,3,2,quality,1,0,w,h,turn);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) assert(clipped[y*w+x]==(x ? gold[turn][y*w+x] : 0));
  }
  const u16 crop[]={5,2,6,3};u16 out[4]={};
  fixed_layout_blit_utils::BlitRgb565SurfaceScaledCrop(out,2,2,2,0,0,2,2,source,6,3,2,1,0,2,2,true,0,0,2,2,1);
  for(int i=0;i<4;++i) assert(out[i]==crop[i]);
  const float xs[]={.2f,.7f,.8f,.3f},ys[]={.3f,.2f,.7f,.8f};
  for(int turn=0;turn<4;++turn) {
    const auto p=image_view::RotatePoint(.2f,.3f,turn);
    assert(std::abs(p.x-xs[turn])<1e-6f && std::abs(p.y-ys[turn])<1e-6f);
    const auto q=image_view::RotatePoint(p.x,p.y,(4-turn)%4);
    assert(std::abs(q.x-.2f)<1e-6f && std::abs(q.y-.3f)<1e-6f);
    const auto size=image_view::VirtualSize(400,240,turn);
    assert(size.width==(turn%2 ? 240 : 400) && size.height==(turn%2 ? 400 : 240));
  }
  puts("PASS: rotated RGB565 gold pixels, crops, clipped sampling and inverse coordinate transforms");
}
