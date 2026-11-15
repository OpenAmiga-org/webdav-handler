/* Unit tests for src/core/charset.c. Public domain. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "charset.h"

static int failures;

static void check(const char *what, const char *got, const char *want)
{
	if (got == NULL || strcmp(got, want) != 0) {
		printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
		failures++;
	}
}

static void roundtrip(int cs, const char *utf8)
{
	char *local = cs_utf8_to_local(cs, utf8);
	char *back = cs_local_to_utf8(cs, local);

	check("roundtrip", back, utf8);
	free(local);
	free(back);
}

int main(void)
{
	char *s;

	s = cs_utf8_to_local(CS_ISO_8859_1, "R\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s");
	check("latin1 out", s, "R\xE4ksm\xF6rg\xE5s");
	free(s);
	s = cs_local_to_utf8(CS_ISO_8859_1, "R\xE4ksm\xF6rg\xE5s");
	check("latin1 in", s, "R\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s");
	free(s);

	/* Euro sign: a byte in ISO-8859-15, an escape in ISO-8859-1 */
	s = cs_utf8_to_local(CS_ISO_8859_15, "5\xE2\x82\xAC");
	check("euro 15", s, "5\xA4");
	free(s);
	s = cs_utf8_to_local(CS_ISO_8859_1, "5\xE2\x82\xAC");
	check("euro 1", s, "5%u20AC");
	free(s);
	/* U+00A4 (currency sign) has no byte in ISO-8859-15 */
	s = cs_utf8_to_local(CS_ISO_8859_15, "\xC2\xA4");
	check("currency 15", s, "%u00A4");
	free(s);

	/* CJK and emoji become escapes and come back */
	s = cs_utf8_to_local(CS_ISO_8859_1, "\xE6\x97\xA5\xE6\x9C\xAC");
	check("cjk", s, "%u65E5%u672C");
	free(s);
	s = cs_utf8_to_local(CS_ISO_8859_1, "\xF0\x9F\x98\x80");
	check("emoji", s, "%U01F600");
	free(s);

	/* a real "%u" in a server name */
	s = cs_utf8_to_local(CS_ISO_8859_1, "100%up");
	check("percent-u", s, "100%u0025up");
	free(s);
	s = cs_utf8_to_local(CS_ISO_8859_1, "50% off");
	check("plain percent", s, "50% off");
	free(s);

	/* ':' cannot be part of an AmigaDOS name, but can be in a message */
	s = cs_utf8_to_name(CS_ISO_8859_1, "12:30 Meeting");
	check("colon in name", s, "12%u003A30 Meeting");
	free(s);
	s = cs_utf8_to_local(CS_ISO_8859_1, "Error: none");
	check("colon in text", s, "Error: none");
	free(s);
	s = cs_utf8_to_message(CS_ISO_8859_1, "Line 1\nLine 2: \xC3\xA5");
	check("message keeps line breaks", s, "Line 1\nLine 2: \xE5");
	free(s);
	s = cs_utf8_to_local(CS_ISO_8859_1, "a\nb");
	check("text escapes line breaks", s, "a%u000Ab");
	free(s);
	s = cs_local_to_utf8(CS_ISO_8859_1, "12%u003A30");
	check("colon back", s, "12:30");
	free(s);

	/* an empty string converts to "", not to whatever the memory held
	 * (sb_steal() used to return an unterminated buffer) */
	{
		int k;

		for (k = 0; k < 4; k++) {
			char *junk = malloc(64);

			strcpy(junk, "leftover text from freed memory");
			free(junk);
			s = k == 0 ? cs_utf8_to_local(CS_ISO_8859_1, "") :
			    k == 1 ? cs_local_to_utf8(CS_ISO_8859_1, "") :
			    k == 2 ? cs_utf8_to_name(CS_ISO_8859_1, "") :
			             cs_utf8_to_message(CS_ISO_8859_1, "");
			check("empty string", s, "");
			free(s);
		}
	}

	roundtrip(CS_ISO_8859_1, "R\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s \xE2\x82\xAC \xE6\x97\xA5 100%up %U");
	roundtrip(CS_ISO_8859_15, "\xC5\xA0koda \xE2\x82\xAC \xC2\xA4 \xF0\x9F\x98\x80");
	roundtrip(CS_ISO_8859_1, "tab\there");

	if (cs_from_name("ISO-8859-15") != CS_ISO_8859_15 || cs_from_name("iso-8859-1") != CS_ISO_8859_1 ||
	    cs_from_name(NULL) != CS_ISO_8859_1) {
		printf("FAIL cs_from_name\n");
		failures++;
	}
	printf("%s\n", failures ? "charset tests FAILED" : "charset tests ok");
	return failures ? 1 : 0;
}
