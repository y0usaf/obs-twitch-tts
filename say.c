#define _GNU_SOURCE
#include <sherpa-onnx/c-api/c-api.h>

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *join(const char *dir, const char *name)
{
	char *path;
	if (asprintf(&path, "%s/%s", dir, name) < 0)
		exit(1);
	return path;
}

static char *readable(char *path)
{
	if (!access(path, R_OK))
		return path;
	free(path);
	return NULL;
}

static char *first_onnx(const char *dir)
{
	DIR *d = opendir(dir);
	char *found = NULL;
	for (struct dirent *e; d && !found && (e = readdir(d));) {
		size_t n = strlen(e->d_name);
		if (n > 5 && !strcmp(e->d_name + n - 5, ".onnx"))
			found = join(dir, e->d_name);
	}
	if (d)
		closedir(d);
	return found;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s MODEL_DIR\n", argv[0]);
		return 2;
	}
	const char *dir = argv[1];
	char *model = first_onnx(dir);
	SherpaOnnxOfflineTtsConfig c = {0};
	c.model.num_threads = THREADS;
	c.model.provider = "cpu";
	char *voices = readable(join(dir, "voices.bin"));
	if (voices) {
		c.model.kokoro.model = model;
		c.model.kokoro.voices = voices;
		c.model.kokoro.tokens = join(dir, "tokens.txt");
		c.model.kokoro.data_dir = join(dir, "espeak-ng-data");
		c.model.kokoro.lexicon = readable(join(dir, "lexicon-us-en.txt"));
		c.model.kokoro.length_scale = 1.0f;
	} else {
		c.model.vits.model = model;
		c.model.vits.tokens = join(dir, "tokens.txt");
		c.model.vits.data_dir = join(dir, "espeak-ng-data");
		c.model.vits.noise_scale = 0.667f;
		c.model.vits.noise_scale_w = 0.8f;
		c.model.vits.length_scale = 1.0f;
	}
	const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&c);
	if (!tts) {
		fprintf(stderr, "%s: no Kokoro or VITS model in %s\n", argv[0], dir);
		return 1;
	}

	SherpaOnnxGenerationConfig g = {.silence_scale = 0.2f, .speed = 1.0f};
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, stdin) > 0) {
		line[strcspn(line, "\n")] = '\0';
		char *text;
		g.sid = (int32_t)strtol(line, &text, 10);
		const SherpaOnnxGeneratedAudio *a =
			SherpaOnnxOfflineTtsGenerateWithConfig(tts, text + (*text == ' '), &g, NULL, NULL);
		uint32_t head[2] = {a ? (uint32_t)a->sample_rate : 0, a ? (uint32_t)a->n : 0};
		fwrite(head, sizeof(head), 1, stdout);
		if (a) {
			fwrite(a->samples, sizeof(float), head[1], stdout);
			SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
		}
		if (fflush(stdout))
			return 1;
	}
	return 0;
}
