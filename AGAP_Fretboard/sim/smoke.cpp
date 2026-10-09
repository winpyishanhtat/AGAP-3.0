#include "../AGAP_Fretboard.ino"
#include <cstdio>
int main() { setup(); for (int i = 0; i < 5; i++) loop(); std::printf("%s", sim::serialOut().c_str()); return 0; }
