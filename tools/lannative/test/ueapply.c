// ueapply <vanilla objdump.txt> <rows.json> <object path> <out file>
// Offline check for object edits: for every property of that object, apply the ops of the rows.json (in file order) to the vanilla property text with the same
// splicer the game uses (uetext.h) and write "<prop> = <text>" lines. Compare the output with a dump of the modded game state to prove the ops reproduce it.
#include <stdio.h>
#include "../rowsjson.h"

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; } size_t g = fread(b, 1, (size_t)n, f); b[g] = 0; fclose(f); return b;
}
int main(int argc, char **argv) {
    if (argc < 5) { puts("usage: ueapply <vanilla objdump.txt> <rows.json> </Game/path.Object> <out file>"); return 2; }
    char *dump = slurp(argv[1]), *js = slurp(argv[2]); if (!dump || !js) { puts("cannot read an input file"); return 2; }
    JDoc d; if (jdoc_parse(&d, js)) { printf("rows.json: %s\n", d.err); return 1; }
    RowOps ro = {0}; ObjOps oo = {0}; char err[300];
    if (!rows_from_json(&d, 0, &ro, &oo, err, sizeof err)) { printf("rows.json: %s\n", err); return 1; }
    FILE *out = fopen(argv[4], "wb"); if (!out) { puts("cannot write the output"); return 2; }
    int applied = 0, failed = 0;
    for (char *line = dump; line && *line;) {                                         // each property line: name : Kind = text
        char *eol = strchr(line, '\n'); if (eol) *eol = 0; size_t ll = strlen(line); if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        char *sep = strstr(line, " = "); char *colon = strstr(line, " : ");
        if (line[0] != '#' && sep && colon && colon < sep) {
            char name[100]; size_t nl = (size_t)(colon - line); if (nl >= sizeof name) nl = sizeof name - 1; memcpy(name, line, nl); name[nl] = 0;
            char *text = _strdup(sep + 3);
            for (int i = 0; i < oo.count; i++) {
                const ObjOp *op = &oo.a[i]; if (strcmp(op->object, argv[3])) continue;
                size_t pn = strcspn(op->path, ".["); if (pn != strlen(name) || strncmp(op->path, name, pn)) continue;
                const char *rest = op->path + pn; char buf[220];
                if (*rest == '[') { char *end; int ix = (int)strtol(rest + 1, &end, 10); snprintf(buf, sizeof buf, "[%d]%s", ix, end[1] == '.' || end[1] == 0 ? end + 1 : end + 1); rest = buf; }
                else if (*rest == '.') rest++;
                char *nt = (!*rest && op->kind == UT_SET) ? _strdup(op->value) : ut_apply(text, rest, (UtOp)op->kind, op->value, err, sizeof err);
                if (!nt) { printf("FAILED %s %s: %s\n", name, op->path, err); failed++; continue; }
                free(text); text = nt; applied++;
            }
            fprintf(out, "%.*s = %s\n", (int)(sep - line), line, text); free(text);              /* keep "name : Kind" exactly as in the dump */
        }
        line = eol ? eol + 1 : NULL;
    }
    fclose(out); printf("%d op(s) applied, %d failed\n", applied, failed); return failed != 0;
}
