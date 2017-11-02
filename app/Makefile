########### define IN and OUT files 
# compile all .c file in pwd
SRCS:=$(wildcard ./*.c)  
OBJS := $(SRCS:.c=.o)
OUT := qmark

########### define CC flag
CFLAGS+= -Wall -Werror

$(warning ####debug: objs=$(OBJS) )

all: $(OUT)

%.o: %.c FORCE
	$(CC) $(CFLAGS) -c -o $@ $^

$(OUT): $(OBJS)
	@echo compile $(OUT)
	$(CC) $(LDFLAGS) -o $@ $^

.PHONY: clean 
clean:
	@rm -rf $(OBJS) $(OUT) *.lib
