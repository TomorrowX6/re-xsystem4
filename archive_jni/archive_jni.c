#include <jni.h>
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "ArchiveJNI", __VA_ARGS__)
#else
#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif

static pthread_once_t g_locale_once = PTHREAD_ONCE_INIT;

static void init_utf8_locale(void)
{
	if (!setlocale(LC_CTYPE, "C.UTF-8")) {
		setlocale(LC_CTYPE, "en_US.UTF-8");
	}
}

static void throw_io_exception(JNIEnv *env, const char *msg)
{
	if ((*env)->ExceptionCheck(env)) {
		return;
	}
	jclass ioExCls = (*env)->FindClass(env, "java/io/IOException");
	if (ioExCls) {
		(*env)->ThrowNew(env, ioExCls, msg ? msg : "Archive extraction error");
		(*env)->DeleteLocalRef(env, ioExCls);
	}
}

static int write_all_fd(int fd, const uint8_t *buf, size_t len, int *out_errno)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (out_errno) {
				*out_errno = errno;
			}
			return -1;
		}
		if (n == 0) {
			if (out_errno) {
				*out_errno = EIO;
			}
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

JNIEXPORT jboolean JNICALL
Java_io_github_kichikuou_xsystem4_ArchiveExtractor_nativeIsFdSeekable(
	JNIEnv *env, jclass clazz, jint fd)
{
	(void)env;
	(void)clazz;
	if (fd < 0) {
		return JNI_FALSE;
	}
	off_t cur = lseek((int)fd, 0, SEEK_CUR);
	if (cur == (off_t)-1) {
		return JNI_FALSE;
	}
	off_t end = lseek((int)fd, 0, SEEK_END);
	if (end == (off_t)-1) {
		return JNI_FALSE;
	}
	if (lseek((int)fd, cur, SEEK_SET) == (off_t)-1) {
		return JNI_FALSE;
	}
	return JNI_TRUE;
}

JNIEXPORT jint JNICALL
Java_io_github_kichikuou_xsystem4_ArchiveExtractor_nativeExtractFd(
	JNIEnv *env, jclass clazz, jint fd, jobject callback)
{
	(void)clazz;
	pthread_once(&g_locale_once, init_utf8_locale);

	if (fd < 0 || callback == NULL) {
		throw_io_exception(env, "Invalid file descriptor or callback");
		return -1;
	}

	jclass cbCls = (*env)->GetObjectClass(env, callback);
	if (!cbCls) {
		return -1;
	}
	jmethodID midOnEntry = (*env)->GetMethodID(env, cbCls, "onEntry", "([BJ)Ljava/lang/String;");
	jmethodID midOnProgress = (*env)->GetMethodID(env, cbCls, "onProgress", "(J)V");
	(*env)->DeleteLocalRef(env, cbCls);
	if (!midOnEntry || !midOnProgress) {
		return -1;
	}

	(void)lseek((int)fd, 0, SEEK_SET);

	struct archive *a = archive_read_new();
	if (!a) {
		throw_io_exception(env, "Out of memory creating archive reader");
		return -1;
	}

	archive_read_support_format_zip(a);
	archive_read_support_format_rar(a);
	archive_read_support_format_rar5(a);
	archive_read_support_format_7zip(a);
	archive_read_support_format_tar(a);
	archive_read_support_filter_all(a);

	int extracted_files = -1;

	if (archive_read_open_fd(a, (int)fd, 131072) != ARCHIVE_OK) {
		const char *err = archive_error_string(a);
		char msg[512];
		snprintf(msg, sizeof(msg), "Cannot open archive: %s", err ? err : "unsupported or corrupt file");
		LOGE("%s", msg);
		throw_io_exception(env, msg);
		goto cleanup;
	}

	struct archive_entry *entry = NULL;
	int64_t total_written = 0;
	int64_t last_reported = 0;
	int count = 0;

	for (;;) {
		int hr = archive_read_next_header(a, &entry);
		if (hr == ARCHIVE_EOF) {
			break;
		}
		if (hr < ARCHIVE_OK && hr != ARCHIVE_WARN) {
			const char *err = archive_error_string(a);
			char msg[512];
			if (archive_read_has_encrypted_entries(a) > 0) {
				snprintf(msg, sizeof(msg), "ENCRYPTED: %s", err ? err : "Archive is password-protected");
			} else {
				snprintf(msg, sizeof(msg), "Archive header error: %s", err ? err : "corrupt or unsupported format");
			}
			LOGE("%s", msg);
			throw_io_exception(env, msg);
			goto cleanup;
		}

		if (archive_entry_is_encrypted(entry)) {
			throw_io_exception(env, "ENCRYPTED: Archive is password-protected");
			goto cleanup;
		}

		mode_t ft = archive_entry_filetype(entry);
		if (ft != AE_IFREG && ft != 0) {
			archive_read_data_skip(a);
			continue;
		}

		const char *raw_path = archive_entry_pathname_utf8(entry);
		if (!raw_path || !*raw_path) {
			raw_path = archive_entry_pathname(entry);
		}
		if (!raw_path || !*raw_path) {
			archive_read_data_skip(a);
			continue;
		}

		size_t path_len = strlen(raw_path);
		if (raw_path[path_len - 1] == '/' || raw_path[path_len - 1] == '\\') {
			archive_read_data_skip(a);
			continue;
		}

		jbyteArray jPathBytes = (*env)->NewByteArray(env, (jsize)path_len);
		if (!jPathBytes) {
			goto cleanup;
		}
		(*env)->SetByteArrayRegion(env, jPathBytes, 0, (jsize)path_len, (const jbyte *)raw_path);

		jlong entry_size = archive_entry_size_is_set(entry) ? (jlong)archive_entry_size(entry) : (jlong)-1;
		jstring jOutPath = (jstring)(*env)->CallObjectMethod(env, callback, midOnEntry, jPathBytes, entry_size);
		(*env)->DeleteLocalRef(env, jPathBytes);
		if ((*env)->ExceptionCheck(env)) {
			if (jOutPath) {
				(*env)->DeleteLocalRef(env, jOutPath);
			}
			goto cleanup;
		}
		if (!jOutPath) {
			archive_read_data_skip(a);
			continue;
		}

		const char *out_path = (*env)->GetStringUTFChars(env, jOutPath, NULL);
		if (!out_path) {
			(*env)->DeleteLocalRef(env, jOutPath);
			goto cleanup;
		}
		int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
		int open_err = errno;
		(*env)->ReleaseStringUTFChars(env, jOutPath, out_path);
		(*env)->DeleteLocalRef(env, jOutPath);

		if (out_fd < 0) {
			char msg[512];
			if (open_err == ENOSPC) {
				snprintf(msg, sizeof(msg), "ENOSPC: No space left on device");
			} else {
				snprintf(msg, sizeof(msg), "Cannot create output file: %s", strerror(open_err));
			}
			LOGE("%s", msg);
			throw_io_exception(env, msg);
			goto cleanup;
		}

		const void *chunk = NULL;
		size_t chunk_len = 0;
		la_int64_t chunk_off = 0;
		int64_t cur_off = 0;

		for (;;) {
			int dr = archive_read_data_block(a, &chunk, &chunk_len, &chunk_off);
			if (dr == ARCHIVE_EOF) {
				break;
			}
			if (dr < ARCHIVE_OK && dr != ARCHIVE_WARN) {
				const char *err = archive_error_string(a);
				char msg[512];
				if (archive_read_has_encrypted_entries(a) > 0) {
					snprintf(msg, sizeof(msg), "ENCRYPTED: %s", err ? err : "Archive is password-protected");
				} else {
					snprintf(msg, sizeof(msg), "Archive data error: %s", err ? err : "corrupt or unsupported compression");
				}
				LOGE("%s", msg);
				close(out_fd);
				throw_io_exception(env, msg);
				goto cleanup;
			}
			if (chunk_off > cur_off) {
				if (lseek(out_fd, (off_t)chunk_off, SEEK_SET) == (off_t)-1) {
					int seek_err = errno;
					close(out_fd);
					throw_io_exception(env, seek_err == ENOSPC ? "ENOSPC: No space left on device" : strerror(seek_err));
					goto cleanup;
				}
				cur_off = chunk_off;
			}
			if (chunk && chunk_len > 0) {
				int write_err = 0;
				if (write_all_fd(out_fd, (const uint8_t *)chunk, chunk_len, &write_err) != 0) {
					close(out_fd);
					char msg[512];
					if (write_err == ENOSPC) {
						snprintf(msg, sizeof(msg), "ENOSPC: No space left on device");
					} else {
						snprintf(msg, sizeof(msg), "Write failed: %s", strerror(write_err));
					}
					LOGE("%s", msg);
					throw_io_exception(env, msg);
					goto cleanup;
				}
				cur_off += (int64_t)chunk_len;
				total_written += (int64_t)chunk_len;
				if (total_written - last_reported >= (2 * 1024 * 1024)) {
					last_reported = total_written;
					(*env)->CallVoidMethod(env, callback, midOnProgress, (jlong)total_written);
					if ((*env)->ExceptionCheck(env)) {
						close(out_fd);
						goto cleanup;
					}
				}
			}
		}

		if (entry_size > cur_off) {
			if (ftruncate(out_fd, (off_t)entry_size) != 0 && errno == ENOSPC) {
				close(out_fd);
				throw_io_exception(env, "ENOSPC: No space left on device");
				goto cleanup;
			}
		}

		if (close(out_fd) != 0 && errno == ENOSPC) {
			throw_io_exception(env, "ENOSPC: No space left on device");
			goto cleanup;
		}
		count++;
	}

	if (total_written != last_reported) {
		(*env)->CallVoidMethod(env, callback, midOnProgress, (jlong)total_written);
		if ((*env)->ExceptionCheck(env)) {
			goto cleanup;
		}
	}

	extracted_files = count;

cleanup:
	archive_read_free(a);
	return (jint)extracted_files;
}
