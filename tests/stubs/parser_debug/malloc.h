#pragma once
// The parser test asserts real string capacities, not host heap counters.
struct mallinfo { int uordblks, fordblks; };
inline struct mallinfo mallinfo() { return {0, 0}; }
