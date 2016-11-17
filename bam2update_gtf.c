/* bam2update_gtf.c
 *   generate junction information based on sam/bam file
 *   then, update existing GTF file
 *   currently, only for single-end long read data
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

int bam2update_gtf_usage(void)
{
    err_printf("\n");
    err_printf("Usage:   %s bam2update_gtf [option] <in.bam> <old.gtf> > new.gtf\n", PROG);
    err_printf("\n");
    err_printf("Notice:  the BAM and GTF files should be sorted in advance.\n");
    err_printf("Options:\n\n");
    err_printf("         -s --source       [STR]    source field in GTF, program, database or project name.\n");
    err_printf("                                    [NONE]\n");
	err_printf("\n");
	return 1;
}

char *fgets_gene(FILE *gfp)
{
    char s[100]; char *line = (char*)_err_malloc(1024);
    while (fgets(line, 1024, gfp) != NULL) {
        if ((sscanf(line, "%*s\t%*s\t%s", s) == 1) && strcmp(s, "gene") == 0) return line;
    }
    return NULL;
}

char *read_gene(char *gtf_line, FILE *gfp, bam_hdr_t *h, gene_t *g, FILE *outfp)
{
    g->trans_n = 0;
    if (gtf_line == NULL) return NULL;
    char ref[100]="\0"; int start, end; char strand;
    char type[20]="\0";
    sscanf(gtf_line, "%s\t%*s\t%*s\t%d\t%d\t%*s\t%c", ref, &start, &end, &strand);
    fputs(gtf_line, outfp);
    // add to gene_t
    trans_t *t = trans_init(1);
    while (fgets(gtf_line, 1024, gfp) != NULL) {
        // trans/exon => add to gene_t and print
        // else(CDS, start, stop, UTR) => print
        sscanf(gtf_line, "%s\t%*s\t%s\t%d\t%d\t%*s\t%c", ref, type, &start, &end, &strand);
        uint8_t is_rev = (strand == '-' ? 1 : 0);
        if (strcmp(type, "gene") == 0) goto ret;
        else if (strcmp(type, "transcript") == 0) {
            // add 
            if (t->exon_n != 0) add_trans(g, *t);
            t->exon_n = 0;
        } else if (strcmp(type, "exon") == 0) {
            // add
            add_exon(t, bam_name2id(h, ref), start, end, is_rev);
        }
        fputs(gtf_line, outfp);
    }
    gtf_line = NULL;
ret:
    trans_free(t);
    return gtf_line;
}

int check_novel(trans_t *t, gene_t *g)
{
    if (g->is_rev != t->is_rev) return 0;
    int i, j, k;
    for (i = 0; i < g->trans_n; ++i) {
        for (j = 0; j < g->trans[i].exon_n; ++j) {
            for (k = 0; k < t->exon_n; ++k) {

            }
        }
    }
    return 1;
}

int bam2update_gtf(int argc, char *argv[])
{
    int c; char src[1024]="NONE";
	while ((c = getopt(argc, argv, "b:s:")) >= 0)
    {
        switch(c)
        {
           case 's':
                strcpy(src, optarg);
                break;
            default:
                err_printf("Error: unknown option: %s.\n", optarg);
                return bam2update_gtf_usage();
                break;
        }
    }
    if (argc - optind != 2) return bam2update_gtf_usage();

    samFile *in; bam_hdr_t *h; bam1_t *b;
    if ((in = sam_open(argv[optind], "rb")) == NULL) err_fatal(__func__, "Cannot open \"%s\"\n", argv[optind]);
    if ((h = sam_hdr_read(in)) == NULL) err_fatal(__func__, "Couldn't read header for \"%s\"\n", argv[optind]);
    b = bam_init1();

    FILE *gfp = fopen(argv[optind+1], "r"); char *gtf_line;
    if ((gtf_line = fgets_gene(gfp)) == NULL) err_fatal(__func__, "Wrong format of GTF file: \"%s\"\n", argv[optind+1]);

    // init for trans/exons from gene in GTF
    gene_t *g = gene_init(); trans_t *t = trans_init(1);
    gtf_line = read_gene(gtf_line, gfp, h, g, stdout);
    // init for trans from sam record
    int sam_ret = sam_read1(in, h, b); gen_trans(b, t); set_trans(t, bam_get_qname(b));

    // merge loop
    while (sam_ret >= 0 && g->trans_n != 0) {
        // TODO: print_trans(t, h, src, outfp); // print all trans
        // sam_end < gene_s: sam++
        if (t->tid < g->tid || (t->tid == g->tid && t->end < g->start)) {
            if ((sam_ret = sam_read1(in, h, b)) >= 0) {
                gen_trans(b, t); set_trans(t, bam_get_qname(b));
            }
        } // gene_e < sam_s: gene++
        else if (t->tid > g->tid || (t->tid == g->tid && t->start > g->end)) {
            gtf_line = read_gene(gtf_line, gfp, h, g, stdout);
        } // overlap and novel: add & merge & print
        else {
            if (check_novel(t, g)) {
                add_trans(g, *t);
                print_trans(*t, h, src, stdout);
            }
        }
    }

    gene_free(g); trans_free(t); free(gtf_line);
    bam_destroy1(b); bam_hdr_destroy(h); sam_close(in); fclose(gfp);
    return 0;
}
