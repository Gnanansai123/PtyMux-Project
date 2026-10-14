CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g
TARGET = ptymux
SRC = src/main.c

.PHONY: all clean run
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
