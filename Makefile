.PHONY: all
all : libcrawler.so file_tester

.PHONY: debug
debug : debug_libcrawler.so file_tester

file_tester : file_tester.c libcrawler.so
	clang -g -L. -lcrawler -lpthread file_tester.c -Wall -Werror -o file_tester

libcrawler.so : crawler.c
	clang -g -fpic -c crawler.c -Wall -Werror -o crawler.o
	clang -g -shared -o libcrawler.so crawler.o

debug_libcrawler.so : crawler.c
	clang -g -fpic -c crawler.c -Wall -Werror -o crawler.o -DDEBUG
	clang -g -shared -o libcrawler.so crawler.o

.PHONY: clean
clean :
	rm -f file_tester libcrawler.so *.o *~
