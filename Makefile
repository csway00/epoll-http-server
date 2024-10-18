src  = $(wildcard *.c)
obj = $(patsubst %.c, %.o, $(src))

tar = server
ALL: $(tar)

$(tar): $(obj)
	gcc $^ -o $@

$(obj): %.o: %.c
	gcc $< -o $@ -c

clean:
	-rm -rf $(obj) $(tar)

.PHONY: clean ALL
