build/test_thunder: tests/test_thunder.c src/thunder.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_thunder $(filter %.c,$^)
