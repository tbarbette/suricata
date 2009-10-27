/* Copyright (c) 2009 Open Information Security Foundation */

/** \file
 *  \author Victor Julien <victor@inliniac.net>
 *  \author Breno Silva <breno.silva@gmail.com>
 */

#include "eidps-common.h"
#include "decode.h"
#include "detect.h"
#include "threads.h"
#include "flow.h"
#include "flow-bit.h"
#include "detect-flowbits.h"
#include "util-binsearch.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"

#include "flow-bit.h"
#include "util-var-name.h"
#include "util-unittest.h"

#define PARSE_REGEX         "([a-z]+)(?:,(.*))?"
static pcre *parse_regex;
static pcre_extra *parse_regex_study;

int DetectFlowbitMatch (ThreadVars *, DetectEngineThreadCtx *, Packet *, Signature *, SigMatch *);
int DetectFlowbitSetup (DetectEngineCtx *, Signature *, SigMatch *, char *);
void DetectFlowbitFree (void *);
void FlowBitsRegisterTests(void);

void DetectFlowbitsRegister (void) {
    sigmatch_table[DETECT_FLOWBITS].name = "flowbits";
    sigmatch_table[DETECT_FLOWBITS].Match = DetectFlowbitMatch;
    sigmatch_table[DETECT_FLOWBITS].Setup = DetectFlowbitSetup;
    sigmatch_table[DETECT_FLOWBITS].Free  = DetectFlowbitFree;
    sigmatch_table[DETECT_FLOWBITS].RegisterTests = FlowBitsRegisterTests;

    const char *eb;
    int eo;
    int opts = 0;

    parse_regex = pcre_compile(PARSE_REGEX, opts, &eb, &eo, NULL);
    if(parse_regex == NULL)
    {
        printf("pcre compile of \"%s\" failed at offset %" PRId32 ": %s\n", PARSE_REGEX, eo, eb);
        goto error;
    }

    parse_regex_study = pcre_study(parse_regex, 0, &eb);
    if(eb != NULL)
    {
        printf("pcre study failed: %s\n", eb);
        goto error;
    }

    return;

error:
    return;
}


static int DetectFlowbitMatchToggle (Packet *p, DetectFlowbitsData *fd) {
    if (p->flow == NULL)
        return 0;

    FlowBitToggle(p->flow,fd->idx);
    return 1;
}

static int DetectFlowbitMatchUnset (Packet *p, DetectFlowbitsData *fd) {
    if (p->flow == NULL)
        return 0;

    FlowBitUnset(p->flow,fd->idx);
    return 1;
}

static int DetectFlowbitMatchSet (Packet *p, DetectFlowbitsData *fd) {
    if (p->flow == NULL)
        return 0;

    FlowBitSet(p->flow,fd->idx);
    return 1;
}

static int DetectFlowbitMatchIsset (Packet *p, DetectFlowbitsData *fd) {
    if (p->flow == NULL)
        return 0;

    return FlowBitIsset(p->flow,fd->idx);
}

static int DetectFlowbitMatchIsnotset (Packet *p, DetectFlowbitsData *fd) {
    if (p->flow == NULL)
        return 0;

    return FlowBitIsnotset(p->flow,fd->idx);
}

/*
 * returns 0: no match
 *         1: match
 *        -1: error
 */

int DetectFlowbitMatch (ThreadVars *t, DetectEngineThreadCtx *det_ctx, Packet *p, Signature *s, SigMatch *m)
{
    DetectFlowbitsData *fd = (DetectFlowbitsData *)m->ctx;
    if (fd == NULL)
        return 0;

    switch (fd->cmd) {
        case DETECT_FLOWBITS_CMD_ISSET:
            return DetectFlowbitMatchIsset(p,fd);
        case DETECT_FLOWBITS_CMD_ISNOTSET:
            return DetectFlowbitMatchIsnotset(p,fd);
        case DETECT_FLOWBITS_CMD_SET:
            return DetectFlowbitMatchSet(p,fd);
        case DETECT_FLOWBITS_CMD_UNSET:
            return DetectFlowbitMatchUnset(p,fd);
        case DETECT_FLOWBITS_CMD_TOGGLE:
            return DetectFlowbitMatchToggle(p,fd);
        default:
            printf("ERROR: DetectFlowbitMatch unknown cmd %" PRIu32 "\n", fd->cmd);
            return 0;
    }

    return 0;
}

int DetectFlowbitSetup (DetectEngineCtx *de_ctx, Signature *s, SigMatch *m, char *rawstr)
{
    DetectFlowbitsData *cd = NULL;
    SigMatch *sm = NULL;
    char *str = rawstr;
    char dubbed = 0;
    char *fb_cmd_str = NULL, *fb_name = NULL;
    uint8_t fb_cmd = 0;
#define MAX_SUBSTRINGS 30
    int ret = 0, res = 0;
    int ov[MAX_SUBSTRINGS];

    ret = pcre_exec(parse_regex, parse_regex_study, rawstr, strlen(rawstr), 0, 0, ov, MAX_SUBSTRINGS);
    if (ret != 2 && ret != 3) {
        printf("ERROR: \"%s\" is not a valid setting for flowbits.\n", rawstr);
        return -1;
    }

    const char *str_ptr;
    res = pcre_get_substring((char *)rawstr, ov, MAX_SUBSTRINGS, 1, &str_ptr);
    if (res < 0) {
        printf("DetectPcreSetup: pcre_get_substring failed\n");
        return -1;
    }
    fb_cmd_str = (char *)str_ptr;

    if (ret == 3) {
        res = pcre_get_substring((char *)rawstr, ov, MAX_SUBSTRINGS, 2, &str_ptr);
        if (res < 0) {
            printf("DetectPcreSetup: pcre_get_substring failed\n");
            return -1;
        }
        fb_name = (char *)str_ptr;
    }

    if (strcmp(fb_cmd_str,"noalert") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_NOALERT;
    } else if (strcmp(fb_cmd_str,"isset") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_ISSET;
    } else if (strcmp(fb_cmd_str,"isnotset") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_ISNOTSET;
    } else if (strcmp(fb_cmd_str,"set") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_SET;
    } else if (strcmp(fb_cmd_str,"unset") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_UNSET;
    } else if (strcmp(fb_cmd_str,"toggle") == 0) {
        fb_cmd = DETECT_FLOWBITS_CMD_TOGGLE;
    } else {
        printf("ERROR: flowbits action \"%s\" is not supported.\n", fb_cmd_str);
        return -1;
    }

    switch(fb_cmd)  {
        case DETECT_FLOWBITS_CMD_NOALERT:
            if(fb_name != NULL)
            goto error;
            s->flags |= SIG_FLAG_NOALERT;
            return 0;
        case DETECT_FLOWBITS_CMD_ISNOTSET:
        case DETECT_FLOWBITS_CMD_ISSET:
        case DETECT_FLOWBITS_CMD_SET:
        case DETECT_FLOWBITS_CMD_UNSET:
        case DETECT_FLOWBITS_CMD_TOGGLE:
            if(fb_name == NULL)
            goto error;
            break;
    }

    cd = malloc(sizeof(DetectFlowbitsData));
    if (cd == NULL) {
        printf("DetectFlowbitsSetup malloc failed\n");
        goto error;
    }

    if (fb_name != NULL) {
        cd->idx = VariableNameGetIdx(de_ctx,fb_name,DETECT_FLOWBITS);
    } else {
        cd->idx = 0;
    }
    cd->cmd = fb_cmd;
    printf("DetectFlowbitSetup: idx %" PRIu32 ", cmd %s, name %s\n", cd->idx, fb_cmd_str, fb_name ? fb_name : "(null)");

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_FLOWBITS;
    sm->ctx = (void *)cd;

    SigMatchAppend(s,m,sm);

    if (dubbed) free(str);
    return 0;

error:
    if (dubbed) free(str);
    if (sm) free(sm);
    return -1;
}

void DetectFlowbitFree (void *ptr) {
    DetectFlowbitsData *fd = (DetectFlowbitsData *)ptr;

    if (fd == NULL)
        return;

    free(fd);
}

#ifdef UNITTESTS
/**
 * \test FlowBitsTestSig01 is a test for a valid noalert flowbits option
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig01(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();

    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Noalert\"; flowbits:noalert,wrongusage; content:\"GET \"; sid:1;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

end:
    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    PatternMatchDestroy(mpm_ctx);

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    return result;
}

/**
 * \test FlowBitsTestSig02 is a test for a valid isset,set,isnotset,unset,toggle flowbits options
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig02(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;
    int error_count = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"isset rule need an option\"; flowbits:isset; content:\"GET \"; sid:1;)");

    if (s == NULL) {
        error_count++;
    }

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"isnotset rule need an option\"; flowbits:isnotset; content:\"GET \"; sid:2;)");

    if (s == NULL) {
        error_count++;
    }

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"set rule need an option\"; flowbits:set; content:\"GET \"; sid:3;)");

    if (s == NULL) {
        error_count++;
    }

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"unset rule need an option\"; flowbits:unset; content:\"GET \"; sid:4;)");

    if (s == NULL) {
        error_count++;
    }

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"toggle rule need an option\"; flowbits:toggle; content:\"GET \"; sid:5;)");

    if (s == NULL) {
        error_count++;
    }

   if(error_count == 5)
    goto end;

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    if (PacketAlertCheck(&p, 1)) {
        goto cleanup;
    }
    if (PacketAlertCheck(&p, 2)) {
        goto cleanup;
    }
    if (PacketAlertCheck(&p, 3)) {
        goto cleanup;
    }
    if (PacketAlertCheck(&p, 4)) {
        goto cleanup;
    }
    if (PacketAlertCheck(&p, 5)) {
        goto cleanup;
    }

    result = 1;

cleanup:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    return result;
}

/**
 * \test FlowBitsTestSig03 is a test for a invalid flowbits option
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig03(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Unknown cmd\"; flowbits:wrongcmd; content:\"GET \"; sid:1;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }


    return result;
}

/**
 * \test FlowBitsTestSig04 is a test check idx value
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig04(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;
    int idx = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"isset option\"; flowbits:isset,fbt; content:\"GET \"; sid:1;)");

    idx = VariableNameGetIdx(de_ctx,"fbt",DETECT_FLOWBITS);

    if (s == NULL || idx != 1) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;

end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    return result;
}

/**
 * \test FlowBitsTestSig05 is a test check noalert flag
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig05(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    int result = 0;

    memset(&th_v, 0, sizeof(th_v));
    memset(&p, 0, sizeof(p));
    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Noalert\"; flowbits:noalert; content:\"GET \"; sid:1;)");

    if (s == NULL || ((s->flags & SIG_FLAG_NOALERT) != SIG_FLAG_NOALERT)) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    result = 1;

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    return result;
}

/**
 * \test FlowBitsTestSig06 is a test set flowbits option
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig06(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    Flow f;
    GenericVar flowvar, *gv = NULL;
    int result = 0;
    int idx = 0;

    memset(&p, 0, sizeof(Packet));
    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(Flow));
    memset(&flowvar, 0, sizeof(GenericVar));

    p.flow = &f;
    p.flow->flowvar = &flowvar;

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Flowbit set\"; flowbits:set,myflow; sid:10;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    idx = VariableNameGetIdx(de_ctx,"myflow",DETECT_FLOWBITS);

    gv = p.flow->flowvar;

    for ( ; gv != NULL; gv = gv->next) {
        if (gv->type == DETECT_FLOWBITS && gv->idx == idx) {
                result = 1;
        }
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    if(gv) GenericVarFree(gv);

    return result;
end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    if(gv) GenericVarFree(gv);

    return result;
}

/**
 * \test FlowBitsTestSig07 is a test unset flowbits option
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig07(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    Flow f;
    GenericVar flowvar, *gv = NULL;
    int result = 0;
    int idx = 0;

    memset(&p, 0, sizeof(Packet));
    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(Flow));
    memset(&flowvar, 0, sizeof(GenericVar));

    p.flow = &f;
    p.flow->flowvar = &flowvar;

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Flowbit set\"; flowbits:set,myflow2; sid:10;)");
    if (s == NULL) {
        goto end;
    }

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Flowbit unset\"; flowbits:unset,myflow2; sid:11;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    idx = VariableNameGetIdx(de_ctx,"myflow",DETECT_FLOWBITS);

    gv = p.flow->flowvar;

    for ( ; gv != NULL; gv = gv->next) {
        if (gv->type == DETECT_FLOWBITS && gv->idx == idx) {
                result = 1;
        }
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    if(gv) GenericVarFree(gv);

    return result;
end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    if(gv) GenericVarFree(gv);

    return result;
}

/**
 * \test FlowBitsTestSig08 is a test toogle flowbits option
 *
 *  \retval 1 on succces
 *  \retval 0 on failure
 */

static int FlowBitsTestSig08(void) {
    uint8_t *buf = (uint8_t *)
                    "GET /one/ HTTP/1.1\r\n"
                    "Host: one.example.org\r\n"
                    "\r\n";
    uint16_t buflen = strlen((char *)buf);
    Packet p;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx;
    Flow f;
    GenericVar flowvar, *gv = NULL;
    int result = 0;
    int idx = 0;

    memset(&p, 0, sizeof(Packet));
    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(Flow));
    memset(&flowvar, 0, sizeof(GenericVar));

    p.flow = &f;
    p.flow->flowvar = &flowvar;

    p.src.family = AF_INET;
    p.dst.family = AF_INET;
    p.payload = buf;
    p.payload_len = buflen;
    p.proto = IPPROTO_TCP;

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }

    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Flowbit set\"; flowbits:set,myflow2; sid:10;)");

    if (s == NULL) {
        goto end;
    }

    s = s->next  = SigInit(de_ctx,"alert ip any any -> any any (msg:\"Flowbit unset\"; flowbits:toggle,myflow2; sid:11;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    PatternMatchPrepare(mpm_ctx, MPM_B2G);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    SigMatchSignatures(&th_v, de_ctx, det_ctx, &p);

    idx = VariableNameGetIdx(de_ctx,"myflow",DETECT_FLOWBITS);

    gv = p.flow->flowvar;

    for ( ; gv != NULL; gv = gv->next) {
        if (gv->type == DETECT_FLOWBITS && gv->idx == idx) {
                result = 1;
        }
    }

    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);

    DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    PatternMatchDestroy(mpm_ctx);
    DetectEngineCtxFree(de_ctx);

    if(gv) GenericVarFree(gv);

    return result;
end:

    if (de_ctx != NULL) {
        SigGroupCleanup(de_ctx);
        SigCleanSignatures(de_ctx);
    }

    if (det_ctx != NULL) {
        DetectEngineThreadCtxDeinit(&th_v, (void *)det_ctx);
    }

    if (de_ctx != NULL) {
        DetectEngineCtxFree(de_ctx);
    }

    if(gv) GenericVarFree(gv);

    return result;
}
#endif /* UNITTESTS */

/**
 * \brief this function registers unit tests for FlowBits
 */
void FlowBitsRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("FlowBitsTestSig01", FlowBitsTestSig01, 0);
    UtRegisterTest("FlowBitsTestSig02", FlowBitsTestSig02, 0);
    UtRegisterTest("FlowBitsTestSig03", FlowBitsTestSig03, 0);
    UtRegisterTest("FlowBitsTestSig04", FlowBitsTestSig04, 1);
    UtRegisterTest("FlowBitsTestSig05", FlowBitsTestSig05, 1);
    UtRegisterTest("FlowBitsTestSig06", FlowBitsTestSig06, 1);
    UtRegisterTest("FlowBitsTestSig07", FlowBitsTestSig07, 0);
    UtRegisterTest("FlowBitsTestSig08", FlowBitsTestSig08, 0);
#endif /* UNITTESTS */
}
