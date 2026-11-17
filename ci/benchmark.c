#ifndef GPT_SOURCE
#define GPT_SOURCE "../gpt/gpt.c"
#endif
#define main gpt_program_main
#include GPT_SOURCE
#undef main

static double now(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void) {
    GPT2 model;
    double start = now();
    gpt2_build_from_checkpoint(&model, "gpt2_124M.bin");
    fprintf(stderr, "model_load_seconds=%.9f\n", now() - start);
    int initial[] = {31373, 612};
    int expected[] = {338, 635, 281, 4998, 3715, 351, 2506, 852};
    int tokens[10];
    // Warm up weights and worker startup; all reported trials exclude loading.
    gpt2_forward(&model, initial, 1, 2);
    puts("round,forward_seconds,generated");
    for (int round = 0; round < 3; round++) {
        tokens[0] = initial[0]; tokens[1] = initial[1];
        start = now();
        for (int t = 2; t < 10; t++) {
            gpt2_forward(&model, tokens, 1, t);
            tokens[t] = sample_mult(model.acts.probs + (t - 1) * model.config.vocab_size,
                                    model.config.vocab_size);
            assert(tokens[t] == expected[t - 2]);
        }
        printf("%d,%.9f,8\n", round, now() - start);
    }
    gpt2_free(&model);
    return 0;
}
