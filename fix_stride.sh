#!/bin/bash
sed -i 's/screen\[y \* display.height + x\]/screen[y * display.width + x]/g' source/core/text.cpp
sed -i 's/screen\[yl \* display.height + x\]/screen[yl * display.width + x]/g' source/core/text.cpp
sed -i 's/screen\[(yh - 1) \* display.height + x\]/screen[(yh - 1) * display.width + x]/g' source/core/text.cpp
sed -i 's/screen\[y \* display.height + xl\]/screen[y * display.width + xl]/g' source/core/text.cpp
sed -i 's/screen\[y \* display.height + xh - 1\]/screen[y * display.width + xh - 1]/g' source/core/text.cpp
sed -i 's/screen\[sy \* display.height + sx\]/screen[sy * display.width + sx]/g' source/core/text.cpp
sed -i 's/screen\[pen.y \* display.height + pen.x\]/screen[pen.y * display.width + pen.x]/g' source/core/text.cpp
