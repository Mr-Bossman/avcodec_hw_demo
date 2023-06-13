gcc -Og -std=c11 -Wall -Wextra -pedantic -Wno-format -g main.c get_hwdevice.c -lm $(pkg-config --cflags --libs libavcodec libavformat libavutil) -o main
