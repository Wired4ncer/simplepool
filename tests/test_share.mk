build/test_share: tests/test_share.c src/share.c src/sha256.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_share $(filter %.c,$^)
