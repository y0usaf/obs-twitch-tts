#define _GNU_SOURCE
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define NS 1000000000ULL
#define LEAD (NS / 20)
#define BUDGET (30 * NS)

OBS_DECLARE_MODULE()

extern char **environ;

struct tts {
	obs_source_t *source;
	pthread_t thread;
	pthread_mutex_t lock;
	char *channel;
	char *model;
	long long speaker;
	volatile long gen;
	volatile bool stop;
};

struct run {
	struct tts *t;
	long gen;
	char *channel;
	char *model;
	long long speaker;
	pid_t pid;
	int voice;
};

static bool halted(struct run *r)
{
	return os_atomic_load_bool(&r->t->stop) || os_atomic_load_long(&r->t->gen) != r->gen;
}

static bool voice_start(struct run *r)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv))
		return false;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, sv[1], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&fa, sv[1], STDOUT_FILENO);
	char speaker[24];
	snprintf(speaker, sizeof(speaker), "%lld", r->speaker);
	char *argv[] = {SAY, r->model, speaker, NULL};
	int err = posix_spawn(&r->pid, SAY, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(sv[1]);
	if (err) {
		close(sv[0]);
		r->pid = 0;
		blog(LOG_WARNING, "[twitch-tts] %s: %s", SAY, strerror(err));
		return false;
	}
	r->voice = sv[0];
	return true;
}

static void voice_stop(struct run *r)
{
	if (!r->pid)
		return;
	kill(r->pid, SIGKILL);
	waitpid(r->pid, NULL, 0);
	close(r->voice);
	r->pid = 0;
}

static bool recv_all(struct run *r, void *buf, size_t n, uint64_t deadline)
{
	struct pollfd p = {.fd = r->voice, .events = POLLIN};
	for (size_t got = 0; got < n;) {
		if (halted(r) || os_gettime_ns() > deadline)
			return false;
		if (poll(&p, 1, 100) <= 0)
			continue;
		ssize_t k = recv(r->voice, (char *)buf + got, n - got, 0);
		if (k <= 0)
			return false;
		got += (size_t)k;
	}
	return true;
}

static void play(struct run *r, const float *pcm, size_t frames, uint32_t rate)
{
	size_t chunk = rate / 50, off = 0;
	uint64_t start = os_gettime_ns();
	for (; off < frames && !halted(r); off += chunk) {
		uint64_t ts = start + off * NS / rate;
		os_sleepto_ns(ts);
		struct obs_source_audio a = {
			.data = {(const uint8_t *)(pcm + off)},
			.frames = (uint32_t)(frames - off < chunk ? frames - off : chunk),
			.speakers = SPEAKERS_MONO,
			.format = AUDIO_FORMAT_FLOAT,
			.samples_per_sec = rate,
			.timestamp = ts + LEAD,
		};
		obs_source_output_audio(r->t->source, &a);
	}
	os_sleepto_ns(start + (off < frames ? off : frames) * NS / rate);
}

static void say(struct run *r, const char *text)
{
	if (!r->pid && !voice_start(r))
		return;
	struct dstr line = {0};
	dstr_printf(&line, "%s\n", text);
	bool sent = send(r->voice, line.array, line.len, MSG_NOSIGNAL) == (ssize_t)line.len;
	dstr_free(&line);

	uint32_t head[2];
	uint64_t deadline = os_gettime_ns() + BUDGET;
	if (!sent || !recv_all(r, head, sizeof(head), deadline)) {
		if (!halted(r))
			blog(LOG_WARNING, "[twitch-tts] %s died or took over 30s; restarting it", SAY);
		voice_stop(r);
		return;
	}
	if (!head[0] || !head[1])
		return;
	float *pcm = bmalloc((size_t)head[1] * sizeof(float));
	if (recv_all(r, pcm, (size_t)head[1] * sizeof(float), deadline))
		play(r, pcm, head[1], head[0]);
	else
		voice_stop(r);
	bfree(pcm);
}

static void line(struct run *r, int fd, char *l)
{
	if (!strncmp(l, "PING ", 5)) {
		l[1] = 'O';
		send(fd, l, strlen(l), MSG_NOSIGNAL);
		send(fd, "\r\n", 2, MSG_NOSIGNAL);
		return;
	}
	char *bang = strchr(l, '!');
	char *cmd = strstr(l, " PRIVMSG #");
	char *msg = cmd ? strstr(cmd, " :") : NULL;
	if (*l != ':' || !bang || !msg || bang > cmd)
		return;
	*bang = '\0';
	struct dstr text = {0};
	dstr_printf(&text, "%s says %s", l + 1, msg + 2);
	say(r, text.array);
	dstr_free(&text);
}

static int dial(void)
{
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *res;
	if (getaddrinfo("irc.chat.twitch.tv", "6667", &hints, &res))
		return -1;
	int fd = -1;
	struct timeval send_timeout = {.tv_sec = 5}, recv_timeout = {.tv_sec = 1};
	for (struct addrinfo *ai = res; ai && fd < 0; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd >= 0 && (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout)) ||
				setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout)) ||
				connect(fd, ai->ai_addr, ai->ai_addrlen))) {
			close(fd);
			fd = -1;
		}
	}
	freeaddrinfo(res);
	return fd;
}

static void session(struct run *r)
{
	int fd = dial();
	if (fd < 0) {
		blog(LOG_WARNING, "[twitch-tts] cannot reach irc.chat.twitch.tv:6667, retrying in 5s");
		return;
	}
	struct dstr hello = {0};
	dstr_printf(&hello, "NICK justinfan12345\r\nJOIN #%s\r\n", r->channel);
	send(fd, hello.array, hello.len, MSG_NOSIGNAL);
	dstr_free(&hello);
	blog(LOG_INFO, "[twitch-tts] reading #%s with %s, speaker %lld", r->channel, r->model, r->speaker);
	voice_start(r);

	char buf[4096];
	size_t len = 0;
	while (!halted(r)) {
		ssize_t n = recv(fd, buf + len, sizeof(buf) - 1 - len, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (n <= 0)
			break;
		len += (size_t)n;
		buf[len] = '\0';
		char *l = buf, *e;
		while ((e = strstr(l, "\r\n"))) {
			*e = '\0';
			line(r, fd, l);
			l = e + 2;
		}
		len -= (size_t)(l - buf);
		memmove(buf, l, len);
		if (len == sizeof(buf) - 1)
			len = 0;
	}
	voice_stop(r);
	close(fd);
	if (!halted(r))
		blog(LOG_WARNING, "[twitch-tts] lost #%s, retrying in 5s", r->channel);
}

static void *run(void *data)
{
	struct tts *t = data;
	while (!os_atomic_load_bool(&t->stop)) {
		pthread_mutex_lock(&t->lock);
		struct run r = {
			.t = t,
			.gen = os_atomic_load_long(&t->gen),
			.channel = bstrdup(t->channel),
			.model = bstrdup(t->model),
			.speaker = t->speaker,
		};
		pthread_mutex_unlock(&t->lock);
		if (*r.channel)
			session(&r);
		bfree(r.channel);
		bfree(r.model);
		for (int i = 0; i < 50 && !halted(&r); i++)
			os_sleep_ms(100);
	}
	return NULL;
}

static void update(void *data, obs_data_t *settings)
{
	struct tts *t = data;
	const char *raw = obs_data_get_string(settings, "channel");
	char *channel = bstrdup(raw + (*raw == '#'));
	for (char *c = channel; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	pthread_mutex_lock(&t->lock);
	bfree(t->channel);
	bfree(t->model);
	t->channel = channel;
	t->model = bstrdup(obs_data_get_string(settings, "model"));
	t->speaker = obs_data_get_int(settings, "speaker");
	os_atomic_inc_long(&t->gen);
	pthread_mutex_unlock(&t->lock);
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	struct tts *t = bzalloc(sizeof(*t));
	t->source = source;
	pthread_mutex_init(&t->lock, NULL);
	update(t, settings);
	if (pthread_create(&t->thread, NULL, run, t)) {
		blog(LOG_ERROR, "[twitch-tts] cannot start thread");
		pthread_mutex_destroy(&t->lock);
		bfree(t->channel);
		bfree(t->model);
		bfree(t);
		return NULL;
	}
	return t;
}

static void destroy(void *data)
{
	struct tts *t = data;
	os_atomic_set_bool(&t->stop, true);
	pthread_join(t->thread, NULL);
	pthread_mutex_destroy(&t->lock);
	bfree(t->channel);
	bfree(t->model);
	bfree(t);
}

static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "model", MODEL);
	obs_data_set_default_int(settings, "speaker", 3);
}

static obs_properties_t *properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "channel", "Twitch channel", OBS_TEXT_DEFAULT);
	obs_properties_add_path(props, "model", "Model directory", OBS_PATH_DIRECTORY, NULL, MODEL);
	obs_properties_add_int(props, "speaker", "Speaker ID", 0, 9999, 1);
	return props;
}

static const char *name(void *data)
{
	UNUSED_PARAMETER(data);
	return "Twitch Chat TTS";
}

static struct obs_source_info source = {
	.id = "twitch_chat_tts",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
	.get_name = name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.get_defaults = defaults,
	.get_properties = properties,
};

bool obs_module_load(void)
{
	obs_register_source(&source);
	return true;
}
