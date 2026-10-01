// nb_lexer.c — ver nb_lexer.h para la explicación de por qué esta
// pieza se reescribe (cadenas dinámicas, no un `char[64]` fijo).
//
// Sin libc -- las funciones de clasificación de caracteres
// (nb_is_digit, etc.) son mínimas y locales a este archivo, ya que
// nblibc.c todavía no se ha vuelto a escribir en esta reescritura.

#include "nb_lexer.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

extern nb_string_t *nb_string_new(const char *data, uint32_t len);

static inline bool nb_is_digit(char c) { return c >= '0' && c <= '9'; }
static inline bool nb_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline bool nb_is_alnum(char c) { return nb_is_digit(c) || nb_is_alpha(c); }
static inline bool nb_is_xdigit(char c) {
    return nb_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static inline char nb_to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

static inline int nb_str_eq_ci(const char *a, const char *b) {
    // 'a' ya viene en mayusculas (ver upper[] en nb_lexer_next); 'b'
    // es el literal con el que se compara, tambien en mayusculas.
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void nb_lexer_init(nb_lexer_t *lx, const char *source) {
    lx->src = source;
    lx->pos = 0;
    lx->line = 1;
}

static char nb_peek(nb_lexer_t *lx) { return lx->src[lx->pos]; }
static char nb_peek2(nb_lexer_t *lx) { return lx->src[lx->pos] ? lx->src[lx->pos + 1] : '\0'; }
// Tercer caracter: hace falta para distinguir "0xFF" (hexadecimal) de
// un simple "0" seguido de otra cosa. Mismo cuidado que nb_peek2: no
// leer mas alla del terminador.
static char nb_peek3(nb_lexer_t *lx) {
    if (!lx->src[lx->pos] || !lx->src[lx->pos + 1]) return '\0';
    return lx->src[lx->pos + 2];
}
static char nb_advance(nb_lexer_t *lx) {
    char c = lx->src[lx->pos];
    if (c != '\0') lx->pos++;
    return c;
}

// Cadena de comparaciones directas, NO una tabla de punteros -- el
// cargador de Nemo OS no reubica punteros de DATOS al copiar el
// programa a su dirección real en memoria (solo el código, vía
// instrucciones adrp relativas a sí mismo). Una tabla estática de
// {puntero_a_cadena, tipo} quedaría con punteros absolutos
// incorrectos en tiempo de ejecución. Mismo motivo, exactamente, que
// documenta H13 en la auditoría del compilador viejo.
static nb_token_type_t nb_lookup_keyword(const char *upper) {
    if (nb_str_eq_ci(upper, "IF")) return TOK_KW_IF;
    if (nb_str_eq_ci(upper, "THEN")) return TOK_KW_THEN;
    if (nb_str_eq_ci(upper, "ELSE")) return TOK_KW_ELSE;
    if (nb_str_eq_ci(upper, "ELSEIF")) return TOK_KW_ELSEIF;
    if (nb_str_eq_ci(upper, "ENDIF")) return TOK_KW_ENDIF;
    if (nb_str_eq_ci(upper, "FOR")) return TOK_KW_FOR;
    if (nb_str_eq_ci(upper, "TO")) return TOK_KW_TO;
    if (nb_str_eq_ci(upper, "STEP")) return TOK_KW_STEP;
    if (nb_str_eq_ci(upper, "NEXT")) return TOK_KW_NEXT;
    if (nb_str_eq_ci(upper, "WHILE")) return TOK_KW_WHILE;
    if (nb_str_eq_ci(upper, "WEND")) return TOK_KW_WEND;
    if (nb_str_eq_ci(upper, "REPEAT")) return TOK_KW_REPEAT;
    if (nb_str_eq_ci(upper, "UNTIL")) return TOK_KW_UNTIL;
    if (nb_str_eq_ci(upper, "FOREVER")) return TOK_KW_FOREVER;
    if (nb_str_eq_ci(upper, "EXIT")) return TOK_KW_EXIT;
    if (nb_str_eq_ci(upper, "GOTO")) return TOK_KW_GOTO;
    if (nb_str_eq_ci(upper, "GOSUB")) return TOK_KW_GOSUB;
    if (nb_str_eq_ci(upper, "FUNCTION")) return TOK_KW_FUNCTION;
    if (nb_str_eq_ci(upper, "RETURN")) return TOK_KW_RETURN;
    if (nb_str_eq_ci(upper, "DIM")) return TOK_KW_DIM;
    if (nb_str_eq_ci(upper, "TYPE")) return TOK_KW_TYPE;
    if (nb_str_eq_ci(upper, "NEW")) return TOK_KW_NEW;
    if (nb_str_eq_ci(upper, "DELETE")) return TOK_KW_DELETE;
    if (nb_str_eq_ci(upper, "FIRST")) return TOK_KW_FIRST;
    if (nb_str_eq_ci(upper, "LAST")) return TOK_KW_LAST;
    if (nb_str_eq_ci(upper, "BEFORE")) return TOK_KW_BEFORE;
    if (nb_str_eq_ci(upper, "AFTER")) return TOK_KW_AFTER;
    if (nb_str_eq_ci(upper, "INSERT")) return TOK_KW_INSERT;
    if (nb_str_eq_ci(upper, "FIELD")) return TOK_KW_FIELD;
    if (nb_str_eq_ci(upper, "EACH")) return TOK_KW_EACH;
    if (nb_str_eq_ci(upper, "NULL")) return TOK_KW_NULL;
    if (nb_str_eq_ci(upper, "GLOBAL")) return TOK_KW_GLOBAL;
    if (nb_str_eq_ci(upper, "LOCAL")) return TOK_KW_LOCAL;
    if (nb_str_eq_ci(upper, "CONST")) return TOK_KW_CONST;
    if (nb_str_eq_ci(upper, "SELECT")) return TOK_KW_SELECT;
    if (nb_str_eq_ci(upper, "CASE")) return TOK_KW_CASE;
    if (nb_str_eq_ci(upper, "DEFAULT")) return TOK_KW_DEFAULT;
    if (nb_str_eq_ci(upper, "DATA")) return TOK_KW_DATA;
    if (nb_str_eq_ci(upper, "READ")) return TOK_KW_READ;
    if (nb_str_eq_ci(upper, "RESTORE")) return TOK_KW_RESTORE;
    if (nb_str_eq_ci(upper, "MOD")) return TOK_KW_MOD;
    if (nb_str_eq_ci(upper, "AND")) return TOK_KW_AND;
    if (nb_str_eq_ci(upper, "OR")) return TOK_KW_OR;
    if (nb_str_eq_ci(upper, "NOT")) return TOK_KW_NOT;
    if (nb_str_eq_ci(upper, "XOR")) return TOK_KW_XOR;
    if (nb_str_eq_ci(upper, "SHL")) return TOK_KW_SHL;
    if (nb_str_eq_ci(upper, "SHR")) return TOK_KW_SHR;
    if (nb_str_eq_ci(upper, "SAR")) return TOK_KW_SAR;
    if (nb_str_eq_ci(upper, "TRUE")) return TOK_KW_TRUE;
    if (nb_str_eq_ci(upper, "FALSE")) return TOK_KW_FALSE;
    if (nb_str_eq_ci(upper, "PRINT")) return TOK_KW_PRINT;
    if (nb_str_eq_ci(upper, "CLS")) return TOK_KW_CLS;
    if (nb_str_eq_ci(upper, "PLOT")) return TOK_KW_PLOT;
    if (nb_str_eq_ci(upper, "LINE")) return TOK_KW_LINE;
    if (nb_str_eq_ci(upper, "RECT")) return TOK_KW_RECT;
    if (nb_str_eq_ci(upper, "COLOR")) return TOK_KW_COLOR;
    if (nb_str_eq_ci(upper, "GRAPHICS")) return TOK_KW_GRAPHICS;
    if (nb_str_eq_ci(upper, "OVAL")) return TOK_KW_OVAL;
    if (nb_str_eq_ci(upper, "TEXT")) return TOK_KW_TEXT;
    if (nb_str_eq_ci(upper, "CREATEWINDOW")) return TOK_KW_CREATEWINDOW;
    if (nb_str_eq_ci(upper, "CONSOLE")) return TOK_KW_CONSOLE;
    if (nb_str_eq_ci(upper, "DESKTOP")) return TOK_KW_DESKTOP;
    if (nb_str_eq_ci(upper, "DELAY")) return TOK_KW_DELAY;
    if (nb_str_eq_ci(upper, "END")) return TOK_KW_END;
    if (nb_str_eq_ci(upper, "TRY")) return TOK_KW_TRY;
    if (nb_str_eq_ci(upper, "CATCH")) return TOK_KW_CATCH;
    if (nb_str_eq_ci(upper, "THROW")) return TOK_KW_THROW;
    return TOK_IDENT; // no es palabra clave -- el llamador decide (queda como TOK_IDENT)
}

static void nb_skip_line_comment(nb_lexer_t *lx) {
    while (nb_peek(lx) != '\0' && nb_peek(lx) != '\n') nb_advance(lx);
}

// ---------------------------------------------------------------------
// El valor de un literal decimal
//
// Entero y fraccion se juntan en un solo int64 y se divide UNA VEZ al
// final. La idea es buena y se conserva: dividir repetidas veces por
// 10.0 acumula error de redondeo en cada paso.
//
// Lo que faltaba era el tope. Un int64 aguanta 18 cifras decimales con
// seguridad (su maximo, 9223372036854775807, tiene 19 pero no llega a
// 9999999999999999999). A partir de ahi el acumulador DA LA VUELTA en
// silencio: ni desbordamiento, ni aviso, ni error de compilacion. El
// programa se construye, corre, y usa otro numero.
//
// Lo destapo escribir pi con veinte decimales:
//
//   3.14159265358979323846   ->  0.0056461610591694638
//   57.295779513082320876    ->  1.9555472919536661
//
// Perder las cifras de mas NO pierde precision: un double solo guarda
// entre 15 y 17 cifras significativas, asi que de la 18 en adelante ya
// no caben en el resultado tampoco. Se descartan a proposito, que es
// lo mismo que hace strtod de la biblioteca estandar.
#define NB_MAX_CIFRAS 18

static double nb_valor_decimal(const char *src, int int_start, int int_len,
                               int frac_start, int frac_len) {
    // Los ceros de delante no son cifras significativas: "007" son tres
    // caracteres y una sola cifra. Sin quitarlos, "0000000000000000005.5"
    // se iria por el camino lento sin ninguna necesidad.
    while (int_len > 0 && src[int_start] == '0') { int_start++; int_len--; }

    if (int_len > NB_MAX_CIFRAS) {
        // Ni la parte entera cabe. Se acumula en double, perdiendo
        // precision a partir de la cifra 17 pero conservando la
        // MAGNITUD, que es lo unico que se puede salvar aqui. La
        // fraccion de un numero tan grande no cambia ni un bit del
        // resultado.
        double v = 0.0;
        for (int i = 0; i < int_len; i++) v = v * 10.0 + (src[int_start + i] - '0');
        return v;
    }

    int64_t combinado = 0;
    double  divisor = 1.0;
    int     cifras = 0;

    for (int i = 0; i < int_len; i++) {
        combinado = combinado * 10 + (src[int_start + i] - '0');
        cifras++;
    }
    for (int i = 0; i < frac_len && cifras < NB_MAX_CIFRAS; i++, cifras++) {
        combinado = combinado * 10 + (src[frac_start + i] - '0');
        divisor = divisor * 10.0;
    }
    return (double)combinado / divisor;
}

nb_token_t nb_lexer_next(nb_lexer_t *lx) {
    nb_token_t tok;
    tok.text = NULL;
    tok.num_value = 0;
    tok.is_float = false;

    for (;;) {
        char c = nb_peek(lx);
        if (c == ' ' || c == '\t' || c == '\r') { nb_advance(lx); continue; }
        if (c == ';') { nb_skip_line_comment(lx); continue; }
        break;
    }

    tok.line = lx->line;
    char c = nb_peek(lx);

    if (c == '\0') { tok.type = TOK_EOF; return tok; }

    if (c == '\n') {
        nb_advance(lx);
        lx->line++;
        tok.type = TOK_NEWLINE;
        return tok;
    }

    // Flotantes que empiezan directamente por el punto (.5, .098) --
    // se distingue de una etiqueta de datos porque tras el punto
    // viene un DIGITO, no una letra.
    if (c == '.' && nb_is_digit(nb_peek2(lx))) {
        nb_advance(lx); // el punto
        int frac_start = (int)lx->pos;
        while (nb_is_digit(nb_peek(lx))) nb_advance(lx);
        int frac_len = (int)lx->pos - frac_start;
        tok.num_value = nb_valor_decimal(lx->src, 0, 0, frac_start, frac_len);
        tok.is_float = true;
        tok.type = TOK_NUMBER;
        return tok;
    }

    // Numeros: enteros o con parte fraccionaria. Combina entero+
    // fraccion en un solo entero y divide UNA VEZ al final -- dividir
    // repetidas veces por 10.0 acumula error de redondeo.
    // Hexadecimal, forma moderna: 0xFF00FF. BlitzBasic solo tenia
    // $FF00FF (arriba), pero 0x es lo que escribe todo el mundo hoy y
    // es la forma que ya aparece en la documentacion del propio
    // sistema (Color 0xRRGGBB). Se aceptan las dos.
    if (c == '0' && (nb_peek2(lx) == 'x' || nb_peek2(lx) == 'X') && nb_is_xdigit(nb_peek3(lx))) {
        nb_advance(lx);  // el 0
        nb_advance(lx);  // la x
        int64_t hexval = 0;
        while (nb_is_xdigit(nb_peek(lx))) {
            char hc = nb_advance(lx);
            int digit;
            if (hc >= '0' && hc <= '9') digit = hc - '0';
            else if (hc >= 'a' && hc <= 'f') digit = hc - 'a' + 10;
            else digit = hc - 'A' + 10;
            hexval = hexval * 16 + digit;
        }
        tok.num_value = (double)hexval;
        tok.type = TOK_NUMBER;
        return tok;
    }

    if (nb_is_digit(c)) {
        int start = (int)lx->pos;
        while (nb_is_digit(nb_peek(lx))) nb_advance(lx);
        int int_len = (int)lx->pos - start;
        bool has_frac = false;
        int frac_start = 0, frac_len = 0;
        if (nb_peek(lx) == '.' && nb_is_digit(nb_peek2(lx))) {
            nb_advance(lx);
            frac_start = (int)lx->pos;
            while (nb_is_digit(nb_peek(lx))) nb_advance(lx);
            frac_len = (int)lx->pos - frac_start;
            has_frac = true;
        }
        tok.num_value = nb_valor_decimal(lx->src, start, int_len,
                                         frac_start, has_frac ? frac_len : 0);
        tok.is_float = has_frac;
        tok.type = TOK_NUMBER;
        return tok;
    }

    // Hexadecimal: $1A2B -- el '$' aqui es PREFIJO de numero (distinto
    // del sufijo "cadena" que va DESPUES de un identificador, Str$).
    // Se distinguen mirando si tras el '$' viene un digito hex.
    if (c == '$' && nb_is_xdigit(nb_peek2(lx))) {
        nb_advance(lx);
        int64_t hexval = 0;
        while (nb_is_xdigit(nb_peek(lx))) {
            char hc = nb_advance(lx);
            int digit;
            if (hc >= '0' && hc <= '9') digit = hc - '0';
            else if (hc >= 'a' && hc <= 'f') digit = hc - 'a' + 10;
            else digit = hc - 'A' + 10;
            hexval = hexval * 16 + digit;
        }
        tok.num_value = (double)hexval;
        tok.type = TOK_NUMBER;
        return tok;
    }

    // Binario: %1001 -- mismo patron que el hexadecimal.
    if (c == '%' && (nb_peek2(lx) == '0' || nb_peek2(lx) == '1')) {
        nb_advance(lx);
        int64_t binval = 0;
        while (nb_peek(lx) == '0' || nb_peek(lx) == '1') {
            char bc = nb_advance(lx);
            binval = binval * 2 + (bc - '0');
        }
        tok.num_value = (double)binval;
        tok.type = TOK_NUMBER;
        return tok;
    }

    // Cadenas: "..." -- una comilla doble dentro se duplica ("").
    // DOS PASADAS: la primera solo mide cuanto va a ocupar el
    // resultado ya des-escapado, para poder reservar la cadena del
    // tamaño EXACTO (nb_string_new_uninit) sin ningun limite fijo de
    // por medio; la segunda copia de verdad.
    if (c == '"') {
        nb_advance(lx);
        uint32_t scan = lx->pos;
        uint32_t out_len = 0;
        while (lx->src[scan] != '\0' && lx->src[scan] != '\n') {
            if (lx->src[scan] == '"') {
                if (lx->src[scan + 1] == '"') { out_len++; scan += 2; continue; }
                break;
            }
            out_len++; scan++;
        }
        extern nb_string_t *nb_string_new_uninit(uint32_t len, char **out_data);
        char *dst;
        nb_string_t *s = nb_string_new_uninit(out_len, &dst);
        while (nb_peek(lx) != '\0' && nb_peek(lx) != '\n') {
            if (nb_peek(lx) == '"') {
                if (nb_peek2(lx) == '"') { *dst++ = '"'; nb_advance(lx); nb_advance(lx); continue; }
                break;
            }
            *dst++ = nb_advance(lx);
        }
        if (nb_peek(lx) == '"') nb_advance(lx); // cierre
        tok.text = s;
        tok.type = TOK_STRING;
        return tok;
    }

    // Etiquetas de datos: ".nombre"
    if (c == '.' && nb_is_alpha(nb_peek2(lx))) {
        nb_advance(lx);
        int start = (int)lx->pos;
        while (nb_is_alnum(nb_peek(lx)) || nb_peek(lx) == '_') nb_advance(lx);
        tok.text = nb_string_new(lx->src + start, lx->pos - (uint32_t)start);
        tok.type = TOK_DATALABEL;
        return tok;
    }

    // Identificadores y palabras clave: letra inicial, luego
    // letras/digitos/guion bajo, con sufijo opcional $ o # que forma
    // parte del propio nombre (asi "nombre$" y "nombre" son variables
    // DISTINTAS). Los sufijos '%' y '.Tipo' son DECORATIVOS y se
    // descartan: el nombre de la variable es lo de delante.
    //
    // LO DE.Tipo CAMBIO EL, y merece explicacion porque
    // antes hacia lo contrario.
    //
    // "variable.Tipo" es la forma de anotar de que Type es una
    // instancia ("c.C = New C"). El ".Tipo" se conservaba dentro del
    // texto del identificador, asi que 'c.C' y 'c' eran DOS VARIABLES
    // DISTINTAS. Consecuencia: este programa compilaba limpio y fallaba
    // al ejecutarse.
    //
    //     Type C : Field n : End Type
    //     c.C = New C
    //     Print c\n          <-- "objeto Null (falta New)"
    //
    // El c de la tercera linea es otra variable, vacia. El error apunta
    // a una linea que esta bien y no dice que el problema es el nombre.
    //
    // Y la anotacion NO SERVIA PARA NADA: el compilador no la lee en
    // ningun sitio -- ni el parser, ni la tabla de simbolos, ni el
    // generador. Solo se pegaba al nombre. O sea que costaba una trampa
    // y no daba nada a cambio.
    //
    // Ahora se descarta igual que el '%', que esta tres lineas mas
    // arriba y ya era decorativo: tener los dos sufijos decorativos
    // tratados de forma distinta en la MISMA funcion era la
    // incoherencia de fondo.
    if (nb_is_alpha(c) || c == '_') {
        int start = (int)lx->pos;
        while (nb_is_alnum(nb_peek(lx)) || nb_peek(lx) == '_') nb_advance(lx);
        int base_end = (int)lx->pos;
        bool has_percent = false;
        if (nb_peek(lx) == '$' || nb_peek(lx) == '#') {
            nb_advance(lx);
            base_end = (int)lx->pos;
        } else if (nb_peek(lx) == '%') {
            nb_advance(lx);
            has_percent = true;
        }
        int after_suffix = (int)lx->pos;
        // La anotacion ".Tipo" se consume --hay que pasar por encima de
        // ella-- pero NO entra en el nombre: 'end' se queda donde
        // acababa el identificador de verdad.
        if (nb_peek(lx) == '.' && nb_is_alpha(nb_peek2(lx))) {
            nb_advance(lx);
            while (nb_is_alnum(nb_peek(lx)) || nb_peek(lx) == '_') nb_advance(lx);
        }
        int end = after_suffix;

        if (!has_percent) {
            tok.text = nb_string_new(lx->src + start, (uint32_t)(end - start));
        } else {
            // el nombre real salta el '%': [start,base_end) + [after_suffix,end)
            uint32_t len1 = (uint32_t)(base_end - start);
            uint32_t len2 = (uint32_t)(end - after_suffix);
            extern nb_string_t *nb_string_new_uninit(uint32_t len, char **out_data);
            char *dst;
            tok.text = nb_string_new_uninit(len1 + len2, &dst);
            for (uint32_t i = 0; i < len1; i++) dst[i] = lx->src[start + i];
            for (uint32_t i = 0; i < len2; i++) dst[len1 + i] = lx->src[after_suffix + i];
        }

        // Mayusculas SOLO para identificar la palabra clave -- el
        // texto del token conserva las mayusculas/minusculas tal
        // como las escribio el programador (para identificadores; las
        // palabras clave no guardan texto en absoluto, ver mas abajo).
        char upper[32]; // ninguna palabra clave real pasa de 11 letras (ENDFUNCTION);
                         // esto NO es el texto del token, solo un buffer de trabajo
                         // para la comparacion, y con limite de tamaño explicito
                         // comprobado a continuacion -- no puede desbordarse.
        uint32_t base_len = (uint32_t)(base_end - start);
        if (base_len < sizeof(upper) - 1) {
            uint32_t i = 0;
            for (; i < base_len; i++) upper[i] = nb_to_upper(lx->src[start + i]);
            upper[i] = '\0';

            nb_token_type_t kw = nb_lookup_keyword(upper);
            if (kw != TOK_IDENT) {
                // Es palabra clave -- el texto que ya reservamos no
                // hace falta para nada (la propia enumeracion basta),
                // asi que lo soltamos para no dejarlo reservado sin uso.
                extern void nb_string_release(nb_string_t *s);
                nb_string_release(tok.text);
                tok.text = NULL;

                // "End Function"/"End Type"/"End Select"/"End If" --
                // dos palabras que el lexer funde en un solo token.
                if (kw == TOK_KW_END) {
                    uint32_t save_pos = lx->pos; int32_t save_line = lx->line;
                    while (nb_peek(lx) == ' ' || nb_peek(lx) == '\t') nb_advance(lx);
                    uint32_t w_start = lx->pos;
                    while (nb_is_alpha(nb_peek(lx))) nb_advance(lx);
                    uint32_t w_len = lx->pos - w_start;
                    char next_upper[16];
                    if (w_len > 0 && w_len < sizeof(next_upper)) {
                        for (uint32_t k = 0; k < w_len; k++) next_upper[k] = nb_to_upper(lx->src[w_start + k]);
                        next_upper[w_len] = '\0';
                        if (nb_str_eq_ci(next_upper, "FUNCTION")) { tok.type = TOK_KW_ENDFUNCTION; return tok; }
                        if (nb_str_eq_ci(next_upper, "TYPE")) { tok.type = TOK_KW_ENDTYPE; return tok; }
                        if (nb_str_eq_ci(next_upper, "SELECT")) { tok.type = TOK_KW_ENDSELECT; return tok; }
                        if (nb_str_eq_ci(next_upper, "IF")) { tok.type = TOK_KW_ENDIF; return tok; }
                    }
                    lx->pos = save_pos; lx->line = save_line;
                }
                tok.type = kw;
                return tok;
            }
        }

        tok.type = TOK_IDENT;
        return tok;
    }

    // Operadores y puntuacion
    nb_advance(lx);
    switch (c) {
        case '+': tok.type = TOK_PLUS; return tok;
        case '-': tok.type = TOK_MINUS; return tok;
        case '*': tok.type = TOK_STAR; return tok;
        case '/': tok.type = TOK_SLASH; return tok;
        case '\\': tok.type = TOK_BACKSLASH; return tok;
        case '(': tok.type = TOK_LPAREN; return tok;
        case ')': tok.type = TOK_RPAREN; return tok;
        case ',': tok.type = TOK_COMMA; return tok;
        case ':': tok.type = TOK_COLON; return tok;
        case '=':
            if (nb_peek(lx) == '>') { nb_advance(lx); tok.type = TOK_GE; return tok; }
            if (nb_peek(lx) == '<') { nb_advance(lx); tok.type = TOK_LE; return tok; }
            tok.type = TOK_EQ; return tok;
        case '<':
            if (nb_peek(lx) == '=') { nb_advance(lx); tok.type = TOK_LE; return tok; }
            if (nb_peek(lx) == '>') { nb_advance(lx); tok.type = TOK_NE; return tok; }
            tok.type = TOK_LT; return tok;
        case '>':
            if (nb_peek(lx) == '=') { nb_advance(lx); tok.type = TOK_GE; return tok; }
            tok.type = TOK_GT; return tok;
        default:
            // Caracter no reconocido -- se devuelve como identificador
            // de un solo caracter para que el parser pueda reportar un
            // error de sintaxis con contexto.
            tok.text = nb_string_new(&c, 1);
            tok.type = TOK_IDENT;
            return tok;
    }
}

const char *nb_token_type_name(nb_token_type_t type) {
    switch (type) {
        case TOK_EOF: return "fin de archivo";
        case TOK_NEWLINE: return "fin de linea";
        case TOK_NUMBER: return "numero";
        case TOK_STRING: return "cadena";
        case TOK_IDENT: return "identificador";
        case TOK_PLUS: return "+";
        case TOK_MINUS: return "-";
        case TOK_STAR: return "*";
        case TOK_SLASH: return "/";
        case TOK_EQ: return "=";
        case TOK_LT: return "<";
        case TOK_GT: return ">";
        case TOK_LE: return "<=";
        case TOK_GE: return ">=";
        case TOK_NE: return "<>";
        case TOK_LPAREN: return "(";
        case TOK_RPAREN: return ")";
        case TOK_COMMA: return ",";
        case TOK_COLON: return ":";
        case TOK_BACKSLASH: return "\\";
        case TOK_KW_IF: return "If";
        case TOK_KW_THEN: return "Then";
        case TOK_KW_ELSE: return "Else";
        case TOK_KW_ELSEIF: return "ElseIf";
        case TOK_KW_ENDIF: return "EndIf";
        case TOK_KW_FOR: return "For";
        case TOK_KW_TO: return "To";
        case TOK_KW_STEP: return "Step";
        case TOK_KW_NEXT: return "Next";
        case TOK_KW_WHILE: return "While";
        case TOK_KW_WEND: return "Wend";
        case TOK_KW_REPEAT: return "Repeat";
        case TOK_KW_UNTIL: return "Until";
        case TOK_KW_FOREVER: return "Forever";
        case TOK_KW_EXIT: return "Exit";
        case TOK_KW_GOTO: return "Goto";
        case TOK_KW_GOSUB: return "Gosub";
        case TOK_KW_FUNCTION: return "Function";
        case TOK_KW_ENDFUNCTION: return "End Function";
        case TOK_KW_RETURN: return "Return";
        case TOK_KW_DIM: return "Dim";
        case TOK_KW_TYPE: return "Type";
        case TOK_KW_ENDTYPE: return "End Type";
        case TOK_KW_NEW: return "New";
        case TOK_KW_DELETE: return "Delete";
        case TOK_KW_FIRST: return "First";
        case TOK_KW_LAST: return "Last";
        case TOK_KW_BEFORE: return "Before";
        case TOK_KW_AFTER: return "After";
        case TOK_KW_INSERT: return "Insert";
        case TOK_KW_FIELD: return "Field";
        case TOK_KW_EACH: return "Each";
        case TOK_KW_NULL: return "Null";
        case TOK_KW_SELECT: return "Select";
        case TOK_KW_CASE: return "Case";
        case TOK_KW_DEFAULT: return "Default";
        case TOK_KW_ENDSELECT: return "End Select";
        case TOK_KW_DATA: return "Data";
        case TOK_KW_READ: return "Read";
        case TOK_KW_RESTORE: return "Restore";
        case TOK_DATALABEL: return "etiqueta de datos";
        case TOK_KW_GLOBAL: return "Global";
        case TOK_KW_LOCAL: return "Local";
        case TOK_KW_CONST: return "Const";
        case TOK_KW_MOD: return "Mod";
        case TOK_KW_AND: return "And";
        case TOK_KW_OR: return "Or";
        case TOK_KW_NOT: return "Not";
        case TOK_KW_XOR: return "Xor";
        case TOK_KW_SHL: return "Shl";
        case TOK_KW_SHR: return "Shr";
        case TOK_KW_SAR: return "Sar";
        case TOK_KW_TRUE: return "True";
        case TOK_KW_FALSE: return "False";
        case TOK_KW_PRINT: return "Print";
        case TOK_KW_CLS: return "Cls";
        case TOK_KW_PLOT: return "Plot";
        case TOK_KW_LINE: return "Line";
        case TOK_KW_RECT: return "Rect";
        case TOK_KW_COLOR: return "Color";
        case TOK_KW_GRAPHICS: return "Graphics";
        case TOK_KW_OVAL: return "Oval";
        case TOK_KW_TEXT: return "Text";
        case TOK_KW_CREATEWINDOW: return "CreateWindow";
        case TOK_KW_CONSOLE: return "Console";
        case TOK_KW_DESKTOP: return "Desktop";
        case TOK_KW_DELAY: return "Delay";
        case TOK_KW_END: return "End";
        case TOK_KW_TRY: return "Try";
        case TOK_KW_CATCH: return "Catch";
        case TOK_KW_ENDTRY: return "End Try";
        case TOK_KW_THROW: return "Throw";
        default: return "?";
    }
}
