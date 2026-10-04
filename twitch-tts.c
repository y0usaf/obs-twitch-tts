#define _GNU_SOURCE
#include <obs-module.h>
#include <obs-frontend-api.h>
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
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define NS 1000000000ULL
#define LEAD (NS / 20)
#define BUDGET (30 * NS)
#define MAX_VOICES 1024
#define ID "twitch_chat_tts"
#define VOICES "0-3,5-7,9,14,16,18,21,22,25,26"

OBS_DECLARE_MODULE()

extern char **environ;

static const char *const tlds[] = {"ai", "app", "co", "com", "de", "dev", "gg", "io", "ly", "net", "org", "ru", "tv", "uk", "xyz"};

struct tts {
	obs_source_t *source;
	pthread_t thread;
	pthread_mutex_t lock;
	char *channel;
	char *model;
	char *voices;
	volatile long gen;
	volatile bool stop;
};

struct run {
	struct tts *t;
	long gen;
	char *channel;
	char *model;
	long voices[MAX_VOICES];
	size_t nvoices;
	obs_data_t *rerolls;
	char *last;
	pid_t pid;
	int voice;
};

static size_t parse_voices(const char *s, long *out)
{
	size_t n = 0;
	while (*s && n < MAX_VOICES) {
		char *end;
		long a = strtol(s, &end, 10), b = a;
		if (end == s) {
			s++;
			continue;
		}
		if (*end == '-') {
			s = end + 1;
			b = strtol(s, &end, 10);
			if (end == s)
				b = a;
		}
		for (long v = a; v <= b && n < MAX_VOICES; v++)
			out[n++] = v;
		s = end;
	}
	return n;
}

static long voice_for(struct run *r, const char *nick)
{
	if (!r->nvoices)
		return 0;
	if (obs_data_has_user_value(r->rerolls, nick)) {
		long chosen = (long)obs_data_get_int(r->rerolls, nick);
		for (size_t i = 0; i < r->nvoices; i++)
			if (r->voices[i] == chosen)
				return chosen;
	}
	uint32_t h = 2166136261u;
	for (const char *c = nick; *c; c++)
		h = (h ^ (uint8_t)*c) * 16777619u;
	return r->voices[h % r->nvoices];
}

static pthread_mutex_t rerolls_lock = PTHREAD_MUTEX_INITIALIZER;

static obs_data_t *rerolls_load(void)
{
	char *path = obs_module_config_path("rerolls.json");
	obs_data_t *rerolls = obs_data_create_from_json_file(path);
	bfree(path);
	return rerolls ? rerolls : obs_data_create();
}

static long reroll(struct run *r, const char *nick)
{
	if (r->nvoices < 2)
		return -1;
	long now = voice_for(r, nick);
	size_t i = 0;
	while (i < r->nvoices && r->voices[i] != now)
		i++;
	long voice = r->voices[(i + 1 + os_gettime_ns() % (r->nvoices - 1)) % r->nvoices];
	char *dir = obs_module_config_path(""), *path = obs_module_config_path("rerolls.json");
	pthread_mutex_lock(&rerolls_lock);
	obs_data_release(r->rerolls);
	r->rerolls = rerolls_load();
	obs_data_set_int(r->rerolls, nick, voice);
	if (os_mkdirs(dir) == MKDIR_ERROR || !obs_data_save_json_pretty_safe(r->rerolls, path, "tmp", NULL))
		blog(LOG_WARNING, "[twitch-tts] cannot save %s", path);
	pthread_mutex_unlock(&rerolls_lock);
	bfree(dir);
	bfree(path);
	return voice;
}

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
	char *argv[] = {SAY, r->model, NULL};
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

static void say(struct run *r, long voice, const char *text)
{
	if (!r->pid && !voice_start(r))
		return;
	struct dstr line = {0};
	dstr_printf(&line, "%ld %s\n", voice, text);
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

static bool is_link(const char *w)
{
	if (strstr(w, "://") || !strncasecmp(w, "www.", 4))
		return true;
	while (*w && !isalnum((unsigned char)*w))
		w++;
	const char *end = w + strcspn(w, "/?#"), *tld = NULL;
	bool path = *end == '/';
	while (end > w && !isalnum((unsigned char)end[-1]))
		end--;
	for (const char *c = w; c < end; c++) {
		if (*c == '.' && c[-1] != '.')
			tld = c + 1;
		else if (!isalnum((unsigned char)*c) && *c != '-')
			return false;
	}
	if (!tld)
		return false;
	size_t n = (size_t)(end - tld);
	for (size_t i = 0; i < n; i++)
		if (!isalpha((unsigned char)tld[i]))
			return false;
	if (n < 2)
		return false;
	if (path)
		return true;
	for (size_t i = 0; i < sizeof(tlds) / sizeof(*tlds); i++)
		if (strlen(tlds[i]) == n && !strncasecmp(tlds[i], tld, n))
			return true;
	return false;
}

static void drop_links(struct dstr *out, char *text)
{
	char *save = NULL;
	for (char *w = strtok_r(text, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
		if (is_link(w))
			continue;
		if (out->len)
			dstr_cat_ch(out, ' ');
		dstr_cat(out, w);
	}
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
	const char *nick = l + 1;
	char *body = msg + 2;
	long voice;
	struct dstr text = {0};
	if (!strncasecmp(body, "!reroll", 7) && (!body[7] || body[7] == ' ')) {
		voice = reroll(r, nick);
		if (voice < 0)
			return;
		dstr_printf(&text, "%s has a new voice", nick);
	} else {
		voice = voice_for(r, nick);
		if (!r->last || strcmp(r->last, nick))
			dstr_printf(&text, "%s says", nick);
		size_t head = text.len;
		drop_links(&text, body);
		if (text.len == head) {
			dstr_free(&text);
			return;
		}
	}
	bfree(r->last);
	r->last = bstrdup(nick);
	say(r, voice, text.array);
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
	blog(LOG_INFO, "[twitch-tts] reading #%s with %s, %zu voices", r->channel, r->model, r->nvoices);
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
		struct run r = {.t = t};
		pthread_mutex_lock(&t->lock);
		r.gen = os_atomic_load_long(&t->gen);
		r.channel = bstrdup(t->channel);
		r.model = bstrdup(t->model);
		r.nvoices = parse_voices(t->voices, r.voices);
		pthread_mutex_unlock(&t->lock);
		if (*r.channel) {
			r.rerolls = rerolls_load();
			session(&r);
			obs_data_release(r.rerolls);
		}
		bfree(r.channel);
		bfree(r.model);
		bfree(r.last);
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
	bfree(t->voices);
	t->channel = channel;
	t->model = bstrdup(obs_data_get_string(settings, "model"));
	t->voices = bstrdup(obs_data_get_string(settings, "voices"));
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
		bfree(t->voices);
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
	bfree(t->voices);
	bfree(t);
}

static void defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "model", MODEL);
	obs_data_set_default_string(settings, "voices", VOICES);
}

static obs_properties_t *properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_text(props, "channel", "Twitch channel", OBS_TEXT_DEFAULT);
	obs_properties_add_path(props, "model", "Model directory", OBS_PATH_DIRECTORY, NULL, MODEL);
	obs_property_t *voices = obs_properties_add_text(props, "voices", "Voices", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(voices, "Speaker IDs like 3 or 0-27 or 2,3,16. Each chatter keeps one of them.");
	return props;
}

static uint32_t global_channel(void)
{
	uint32_t channel = 0;
	for (uint32_t i = 0; i < MAX_CHANNELS; i++) {
		obs_source_t *input = obs_get_output_source(i);
		if (!input) {
			if (!channel && i >= 7)
				channel = i;
			continue;
		}
		bool exists = !strcmp(obs_source_get_unversioned_id(input), ID);
		obs_source_release(input);
		if (exists)
			return 0;
	}
	if (!channel)
		blog(LOG_WARNING, "[twitch-tts] no free global audio channel");
	return channel;
}

static void load(void *data, obs_data_t *settings)
{
	if (!obs_data_get_bool(settings, "global"))
		return;
	struct tts *t = data;
	uint32_t channel = global_channel();
	if (channel)
		obs_set_output_source(channel, t->source);
}

static const char *name(void *data)
{
	UNUSED_PARAMETER(data);
	return "Twitch Chat TTS";
}

static struct obs_source_info source = {
	.id = ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CAP_DISABLED,
	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,
	.get_name = name,
	.create = create,
	.destroy = destroy,
	.update = update,
	.load = load,
	.get_defaults = defaults,
	.get_properties = properties,
};

static bool legacy_input(void *data, obs_source_t *input)
{
	if (strcmp(obs_source_get_unversioned_id(input), ID))
		return true;
	obs_data_t *settings = obs_source_get_settings(input);
	bool legacy = !obs_data_get_bool(settings, "global");
	obs_data_release(settings);
	bool *exists = data;
	*exists = legacy;
	return !legacy;
}

static void frontend_event(enum obs_frontend_event event, void *data)
{
	UNUSED_PARAMETER(data);
	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING && event != OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED)
		return;
	uint32_t channel = global_channel();
	if (!channel)
		return;
	bool legacy = false;
	obs_enum_sources(legacy_input, &legacy);
	if (legacy) {
		blog(LOG_WARNING, "[twitch-tts] remove existing scene TTS sources and reload the collection to enable the global input");
		return;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "global", true);
	obs_source_t *input = obs_source_create(source.id, name(NULL), settings, NULL);
	obs_data_release(settings);
	if (!input) {
		blog(LOG_ERROR, "[twitch-tts] cannot create global audio input");
		return;
	}
	obs_set_output_source(channel, input);
	obs_source_release(input);
}

bool obs_module_load(void)
{
	obs_register_source(&source);
	obs_frontend_add_event_callback(frontend_event, NULL);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, NULL);
}
