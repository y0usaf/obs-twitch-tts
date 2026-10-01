#define _GNU_SOURCE
#include <obs-module.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/threading.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
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

OBS_DECLARE_MODULE()

extern char **environ;

struct tts {
	obs_source_t *source;
	pthread_t thread;
	pthread_mutex_t lock;
	char *channel;
	volatile long gen;
	volatile bool stop;
};

static bool halted(struct tts *t, long gen)
{
	return os_atomic_load_bool(&t->stop) || os_atomic_load_long(&t->gen) != gen;
}

static void play(struct tts *t, long gen, const uint8_t *pcm, size_t frames, uint32_t rate)
{
	size_t chunk = rate / 50, off = 0;
	uint64_t start = os_gettime_ns();
	for (; off < frames && !halted(t, gen); off += chunk) {
		uint64_t ts = start + off * NS / rate;
		os_sleepto_ns(ts);
		struct obs_source_audio a = {
			.data = {pcm + off * 2},
			.frames = (uint32_t)(frames - off < chunk ? frames - off : chunk),
			.speakers = SPEAKERS_MONO,
			.format = AUDIO_FORMAT_16BIT,
			.samples_per_sec = rate,
			.timestamp = ts + LEAD,
		};
		obs_source_output_audio(t->source, &a);
	}
	os_sleepto_ns(start + (off < frames ? off : frames) * NS / rate);
}

static void say(struct tts *t, long gen, const char *text)
{
	int out[2];
	if (pipe2(out, O_CLOEXEC))
		return;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
	char *argv[] = {"flite", "-voice", "slt", "-o", "/dev/stdout", "-t", (char *)text, NULL};
	pid_t pid;
	int err = posix_spawn(&pid, FLITE, &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(out[1]);
	if (err) {
		close(out[0]);
		blog(LOG_WARNING, "[twitch-tts] %s: %s", FLITE, strerror(err));
		return;
	}

	uint8_t *wav = NULL;
	size_t len = 0, cap = 0;
	uint64_t deadline = os_gettime_ns() + 10 * NS;
	struct pollfd p = {.fd = out[0], .events = POLLIN};
	for (;;) {
		if (halted(t, gen) || os_gettime_ns() > deadline) {
			kill(pid, SIGKILL);
			len = 0;
			break;
		}
		if (poll(&p, 1, 100) <= 0)
			continue;
		if (len == cap)
			wav = brealloc(wav, cap = cap ? cap * 2 : 1 << 16);
		ssize_t n = read(out[0], wav + len, cap - len);
		if (n <= 0)
			break;
		len += (size_t)n;
	}
	close(out[0]);
	waitpid(pid, NULL, 0);

	uint16_t channels = 0, bits = 0;
	uint32_t rate = 0;
	if (len > 44) {
		memcpy(&channels, wav + 22, 2);
		memcpy(&rate, wav + 24, 4);
		memcpy(&bits, wav + 34, 2);
	}
	if (len > 44 && !memcmp(wav, "RIFF", 4) && !memcmp(wav + 36, "data", 4) && channels == 1 && bits == 16 &&
	    rate >= 1000)
		play(t, gen, wav + 44, (len - 44) / 2, rate);
	else if (!halted(t, gen))
		blog(LOG_WARNING, "[twitch-tts] flite gave no 16-bit mono wav");
	bfree(wav);
}

static void line(struct tts *t, long gen, int fd, char *l)
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
	say(t, gen, text.array);
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

static void session(struct tts *t, long gen, const char *channel)
{
	int fd = dial();
	if (fd < 0) {
		blog(LOG_WARNING, "[twitch-tts] cannot reach irc.chat.twitch.tv:6667, retrying in 5s");
		return;
	}
	struct dstr hello = {0};
	dstr_printf(&hello, "NICK justinfan12345\r\nJOIN #%s\r\n", channel);
	send(fd, hello.array, hello.len, MSG_NOSIGNAL);
	dstr_free(&hello);
	blog(LOG_INFO, "[twitch-tts] reading #%s", channel);

	char buf[4096];
	size_t len = 0;
	while (!halted(t, gen)) {
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
			line(t, gen, fd, l);
			l = e + 2;
		}
		len -= (size_t)(l - buf);
		memmove(buf, l, len);
		if (len == sizeof(buf) - 1)
			len = 0;
	}
	close(fd);
	if (!halted(t, gen))
		blog(LOG_WARNING, "[twitch-tts] lost #%s, retrying in 5s", channel);
}

static void *run(void *data)
{
	struct tts *t = data;
	while (!os_atomic_load_bool(&t->stop)) {
		pthread_mutex_lock(&t->lock);
		char *channel = bstrdup(t->channel);
		long gen = os_atomic_load_long(&t->gen);
		pthread_mutex_unlock(&t->lock);
		if (*channel)
			session(t, gen, channel);
		bfree(channel);
		for (int i = 0; i < 50 && !halted(t, gen); i++)
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
	t->channel = channel;
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
	bfree(t);
}

static obs_properties_t *properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "channel", "Twitch channel", OBS_TEXT_DEFAULT);
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
	.get_properties = properties,
};

bool obs_module_load(void)
{
	obs_register_source(&source);
	return true;
}
