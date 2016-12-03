/* update_gtf.c
 *   generate junction information based on sam/bam file
 *   then, update existing GTF file
 *   currently, only work for single-end long read data
 * 
 * Author:  Yan Gao
 * Contact: yangao07@hit.edu.cn                             */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include "htslib/htslib/sam.h"
#include "utils.h"
#include "gtf.h"

#define bam_unmap(b) ((b)->core.flag & BAM_FUNMAP)

extern const char PROG[20];
extern int gen_exon(trans_t *t, bam1_t *b, uint32_t *c, uint32_t n_cigar);
extern int gen_trans(bam1_t *b, trans_t *t);

int update_gtf_usage(void)
{
    err_printf("\n");
    err_printf("Usage:   %s update-gtf [option] <in.bam> <old.gtf> > new.gtf\n\n", PROG);
    err_printf("Notice:  the BAM and GTF files should be sorted in advance.\n\n");
    err_printf("Options:\n\n");
    err_printf("         -d --distance    [INT]    consider same if distance between two splice site is less than d. [%d]\n", SPLICE_DISTANCE);
    err_printf("         -l --full-length [INT]    level of strict criterion for considering full-length transcript. \n");
    err_printf("                                   (1->5, most strict->most relaxed) [%d]\n", 5);
    err_printf("         -u --unclassified         output UNCLASSIFIED novel transcript. [false]\n");
    err_printf("         -s --source      [STR]    source field in GTF, program, database or project name. [NONE]\n");
    err_printf("         -f --full-gtf    [STR]    use this option to output the full GTF information of SAM/BAM to file. [false].\n");
	err_printf("\n");
	return 1;
}

char *fgets_gene(FILE *gfp, char *gtf_line, size_t size)
{
    char s[100];
    while (fgets(gtf_line, size, gfp) != NULL) {
        if (gtf_line[0] == '#') continue;
        if ((sscanf(gtf_line, "%*s\t%*s\t%s", s) == 1) && strcmp(s, "gene") == 0) return gtf_line;
    }
    return NULL;
}

int read_gene(char gtf_line[], FILE *gfp, bam_hdr_t *h, gene_t *g, char ***line, int *line_n, int *line_m)
{
    g->trans_n = g->anno_tran_n = 0; *line_n = 0;
    if (gtf_line == NULL) return 0;
    char ref[100]="\0", type[20]="\0"; int start, end; char strand, add_info[1024];

    sscanf(gtf_line, "%s\t%*s\t%*s\t%d\t%d\t%*s\t%c\t%*s\t%[^\n]", ref, &start, &end, &strand, add_info);
    g->tid = bam_name2id(h, ref), g->start = start, g->end = end, g->is_rev = (strand == '-' ? 1 : 0);
    char tag[10] = "gene_id";
    gtf_add_info(add_info, tag, g->gname);
    //fputs(gtf_line, outfp);
    if (*line_n == *line_m) {
        *line_m <<= 1;
        *line = (char**)_err_realloc(*line, *line_m * sizeof(char*));
        int i;
        for (i=*line_m>>1; i < *line_m; ++i)
            (*line)[i] = (char*)_err_malloc(1024);
    }
    strcpy((*line)[*line_n], gtf_line); (*line_n)++;
    // add to gene_t
    trans_t *t = trans_init(1);
    while (fgets(gtf_line, 1024, gfp) != NULL) {
        // trans/exon => add to gene_t and print
        // else(CDS, start, stop, UTR) => print
        sscanf(gtf_line, "%s\t%*s\t%s\t%d\t%d\t%*s\t%c", ref, type, &start, &end, &strand);
        uint8_t is_rev = (strand == '-' ? 1 : 0);
        if (strcmp(type, "gene") == 0) {
            if (t->exon_n != 0) add_trans(g, *t, 0);
            goto ret;
        } else if (strcmp(type, "transcript") == 0) {
            if (t->exon_n != 0) add_trans(g, *t, 0);
            t->is_rev = is_rev; t->tid = bam_name2id(h, ref); t->start = start, t->end = end;
            t->exon_n = 0;
        } else if (strcmp(type, "exon") == 0) {
            add_exon(t, bam_name2id(h, ref), start, end, is_rev);
        }
        //fputs(gtf_line, outfp);
        if (*line_n == *line_m) {
            *line_m <<= 1;
            *line = (char**)_err_realloc(*line, *line_m * sizeof(char*));
            int i;
            for (i=*line_m>>1; i < *line_m; ++i)
                (*line)[i] = (char*)_err_malloc(1024);
        }
        strcpy((*line)[*line_n], gtf_line); (*line_n)++;
    }
    if (t->exon_n != 0) add_trans(g, *t, 0);
    gtf_line = NULL;
ret:
    trans_free(t);
    g->anno_tran_n = g->trans_n;
    if (gtf_line == NULL) return 0;
    else return 1;
}

int check_overlap(char gtf_line[], gene_t _g, bam_hdr_t *h)
{
    gene_t *g = gene_init();
    if (gtf_line == NULL) _err_fatal_simple(__func__, "gtf_line is NULL.\n");
    char ref[100]="\0"; int start, end;

    sscanf(gtf_line, "%s\t%*s\t%*s\t%d\t%d", ref, &start, &end);
    g->tid = bam_name2id(h, ref), g->start = start, g->end = end;

    int ret = 1;
    if (g->tid != _g.tid || g->start > _g.end) ret = 0;
    gene_free(g);
    return ret;
}

// read all ovelaped gene into one group
int read_gene_group(char gtf_line[], FILE *gfp, bam_hdr_t *h, gene_group_t *gg, char ***group_line, int *group_line_m, int **group_line_n, int *group_line_n_m)
{
    char **line = (char**)_err_malloc(sizeof(char*)); line[0] = (char*)_err_malloc(1024);
    int line_n = 0, line_m = 1; int i;
    int tot_line_n = 0;
    gg->gene_n = 0;
    while (read_gene(gtf_line, gfp, h, gg->g+gg->gene_n, &line, &line_n, &line_m)) {
        if (tot_line_n+line_n > *group_line_m) {
            *group_line = (char**)_err_realloc(*group_line, (tot_line_n+line_n) * sizeof(char*));
            for (i=*group_line_m; i < tot_line_n+line_n; ++i)
                (*group_line)[i] = (char*)_err_malloc(1024);
            *group_line_m = tot_line_n+line_n;
        }
        for (i = tot_line_n; i < tot_line_n+line_n; ++i)
            strcpy((*group_line)[i], line[i-tot_line_n]);
        tot_line_n += line_n;
        if (gg->gene_n == *group_line_n_m) {
            *group_line_n_m <<= 1;
            *group_line_n = (int*)_err_realloc(*group_line_n, *group_line_n_m * sizeof(int));
        }
        (*group_line_n)[gg->gene_n] = line_n;
        gg->gene_n++;
        if (gg->gene_n == gg->gene_m) gg = gene_group_realloc(gg);

        if (check_overlap(gtf_line, gg->g[gg->gene_n-1], h) == 0) break;
    }
    set_gene_group(gg);
    for (i = 0; i < line_m; ++i) free(line[i]); free(line);
    return gg->gene_n;
}

int check_full(trans_t t, trans_t anno_t, int level)
{
    int i = t.exon_n-1, j = anno_t.exon_n-1;
    if (level == 1) {
        if (t.is_rev) {
            if (t.exon[i].end != anno_t.exon[i].end) return 0;
            if (t.exon[0].start != anno_t.exon[0].start) return 0;
        } else {
            if (t.exon[0].end != anno_t.exon[0].end) return 0;
            if (t.exon[i].start != anno_t.exon[j].start) return 0;
        }
    } else if (level == 2) {
        if (t.exon[0].start > anno_t.exon[0].end || anno_t.exon[0].start > t.exon[0].end) return 0;
        if (t.exon[i].start > anno_t.exon[j].end || anno_t.exon[j].start > t.exon[i].end) return 0;
    } 
    return 1;
}

// check if t is novel and has identical splice site
// if t has all identical splice sites with other novel t, merge two ends
// @return value
//    0: novel, NOT share any identical splice site (unclassified)
//    1: novel, and share identical splice site (gene_id)
//    2: totally identical, can NOT be added to any anno
//    3: other cases that cannot be added to this anno(not full-length to any anno-trans)
int check_novel(trans_t t, gene_t *g, int dis, int l)
{
    if (g->is_rev != t.is_rev || t.exon_n < 2) return 3; // different strand: can NOT be added to the anno
                                                           // one-exon transcript
    int anno_t_n = g->anno_tran_n;
    int i, j, k, iden_n=0, iden1=0, full=0;

    if (t.is_rev) { // '-' strand
        for (i = 0; i < g->trans_n; ++i) {
            if (check_full(t, g->trans[i], l) == 0) continue; else full=1;
            iden_n = 0;
            for (k = 0; k < t.exon_n-1; ++k) {
                for (j = 0; j < g->trans[i].exon_n-1; ++j) {
                    if (abs(g->trans[i].exon[j].start - t.exon[k].start) <= dis
                     && abs(g->trans[i].exon[j+1].end - t.exon[k+1].end) <= dis) {
                        iden_n++;
                        break;
                    }
                }
            }
            // check
            if (iden_n > 0 && i < anno_t_n) iden1=1;
            if (t.exon_n == g->trans[i].exon_n && iden_n == t.exon_n-1) {
                if (i >= anno_t_n) { // merge
                    g->trans[i].cov++;
                    if (g->trans[i].exon[0].end < t.end)
                        g->trans[i].end = g->trans[i].exon[0].end = t.end;
                    if (g->trans[i].exon[t.exon_n-1].start > t.start) 
                        g->trans[i].start = g->trans[i].exon[t.exon_n-1].start = t.start;
                }
                return 2;
            }
        }
    } else { // '+' strand
        for (i = 0; i < g->trans_n; ++i) {
            if (check_full(t, g->trans[i], l) == 0) continue; else full=1;
            iden_n = 0;
            for (k = 0; k < t.exon_n-1; ++k) {
                for (j = 0; j < g->trans[i].exon_n-1; ++j) {
                    if (abs(g->trans[i].exon[j].end - t.exon[k].end) <= dis 
                     && abs(g->trans[i].exon[j+1].start - t.exon[k+1].start) <= dis) {
                        iden_n++;
                        break;
                    }
                }
            }
            // check
            if (iden_n > 0 && i < anno_t_n) iden1=1;
            if (t.exon_n == g->trans[i].exon_n && iden_n == t.exon_n-1) {
                if (i >= anno_t_n) { // merge
                    g->trans[i].cov++;
                    if (g->trans[i].exon[0].start > t.start)
                        g->trans[i].start = g->trans[i].exon[0].start = t.start;
                    if (g->trans[i].exon[t.exon_n-1].end < t.end)
                        g->trans[i].end = g->trans[i].exon[t.exon_n-1].end = t.end;
                }
                return 2;
            }
        }
    }
    if (full) return iden1;
    else return 3;
}

void group_check_novel(trans_t t, gene_group_t *gg, int dis, int uncla, int l)
{
    int i;
    int gene_i = -1, novel_gene_i=-1;
    for (i = 0; i < gg->gene_n; ++i) {
        int ret = check_novel(t, gg->g+i, dis, l);
        if (ret == 0) {
            if (novel_gene_i < 0) novel_gene_i = i;
        } else if (ret == 1) {
            if (gene_i < 0) gene_i = i;
        } else if (ret == 2) return;
    }
    // not share any identical splice-site
    if (gene_i >= 0) {
        add_trans(gg->g+gene_i, t, 0);
    } else if (uncla && novel_gene_i > 0){ // UNCLASSIFIED
        add_trans(gg->g+novel_gene_i, t, 1);
    }
}

const struct option update_long_opt [] = {
    { "distance", 1, NULL, 'd' },
    { "unclassified", 0, NULL, 'u' },
    { "source", 1, NULL, 's' },
    { "full-gtf", 1, NULL, 'f' },

    { 0, 0, 0, 0}
};

int update_gtf(int argc, char *argv[])
{
    int c; int dis=SPLICE_DISTANCE, l=5, uncla = 0; char src[1024]="NONE"; FILE *new_gfp=stdout, *full_gfp=NULL;
	while ((c = getopt_long(argc, argv, "d:l:us:f:", update_long_opt, NULL)) >= 0) {
        switch(c)
        {
            case 'd': dis = atoi(optarg); break;
            case 'l': l = atoi(optarg); break;
            case 'u': uncla = 1;
            case 's': strcpy(src, optarg); break;
            case 'f': if ((full_gfp = fopen(optarg, "w")) == NULL) {
                          err_fatal(__func__, "Can not open full-gtf output file \"%s\"\n", optarg);
                          return update_gtf_usage();
                      }
                      break;
            default:
                      err_printf("Error: unknown option: %s.\n", optarg);
                      return update_gtf_usage();
                      break;
        }
    }
    if (argc - optind != 2) return update_gtf_usage();

    samFile *in; bam_hdr_t *h; bam1_t *b; trans_t *t; 
    if ((in = sam_open(argv[optind], "rb")) == NULL) err_fatal(__func__, "Cannot open \"%s\"\n", argv[optind]);
    if ((h = sam_hdr_read(in)) == NULL) err_fatal(__func__, "Couldn't read header for \"%s\"\n", argv[optind]);
    b = bam_init1(); t = trans_init(1);

    FILE *gfp = fopen(argv[optind+1], "r"); char *gtf_line = (char*)_err_malloc(1024);
    if ((gtf_line = fgets_gene(gfp, gtf_line, 1024)) == NULL) err_fatal(__func__, "Wrong format of GTF file: \"%s\"\n", argv[optind+1]);
    char **group_line; int group_line_m; int *group_line_n, group_line_n_m; // anno lines
    group_line_m = group_line_n_m = 1; group_line_n = (int*)_err_calloc(1, sizeof(int));
    group_line = (char**)_err_malloc(sizeof(char*)); *group_line = (char*)_err_malloc(1024 * sizeof(char));

    // init for trans/exons from gene in GTF
    gene_group_t *gg = gene_group_init();
    read_gene_group(gtf_line, gfp, h, gg, &group_line, &group_line_m, &group_line_n, &group_line_n_m);
    // init for trans from sam record
    int sam_ret = sam_read1(in, h, b); 
    if (sam_ret >= 0) {
        gen_trans(b, t); set_trans(t, bam_get_qname(b));
        if (full_gfp) print_trans(*t, h, src, full_gfp);
    }

    // merge loop
    while (sam_ret >= 0 && gg->gene_n != 0) {
        // sam_end < gene_s: sam++
        if (t->tid < gg->tid || (t->tid == gg->tid && t->end < gg->start)) {
            if ((sam_ret = sam_read1(in, h, b)) >= 0) {
                gen_trans(b, t); set_trans(t, bam_get_qname(b));
                if (full_gfp) print_trans(*t, h, src, full_gfp);
            }
        } // gene_e < sam_s: gene++
        else if (t->tid > gg->tid || (t->tid == gg->tid && t->start > gg->end)) {
            print_gene_group(*gg, h, src, new_gfp, group_line, group_line_n);
            read_gene_group(gtf_line, gfp, h, gg, &group_line, &group_line_m, &group_line_n, &group_line_n_m);
        } else { // overlap and novel: add & merge & print
            group_check_novel(*t, gg, dis, uncla, l);
            if ((sam_ret = sam_read1(in, h, b)) >= 0) {
                gen_trans(b, t); set_trans(t, bam_get_qname(b));
                if (full_gfp) print_trans(*t, h, src, full_gfp);
            }
        }
    }
    print_gene_group(*gg, h, src, new_gfp, group_line, group_line_n);

    // just print all the remaining anno-line
    fputs(gtf_line, new_gfp);
    while (fgets(gtf_line, 1024, gfp) != NULL)
        fputs(gtf_line, new_gfp);
    while (full_gfp && sam_ret >= 0) {
        if ((sam_ret = sam_read1(in, h, b)) >= 0) {
            gen_trans(b, t); set_trans(t, bam_get_qname(b));
            print_trans(*t, h, src, full_gfp);
        }
    }

    int i;
    for (i = 0; i < group_line_m; ++i) free(group_line[i]); free(group_line); free(group_line_n);
    gene_group_free(gg); trans_free(t); free(gtf_line);
    bam_destroy1(b); bam_hdr_destroy(h); sam_close(in); fclose(gfp); if(full_gfp) fclose(full_gfp);
    return 0;
}
