/*___INFO__MARK_BEGIN__*/
/*************************************************************************
 * 
 *  The Contents of this file are made available subject to the terms of
 *  the Sun Industry Standards Source License Version 1.2
 * 
 *  Sun Microsystems Inc., March, 2001
 * 
 * 
 *  Sun Industry Standards Source License Version 1.2
 *  =================================================
 *  The contents of this file are subject to the Sun Industry Standards
 *  Source License Version 1.2 (the "License"); You may not use this file
 *  except in compliance with the License. You may obtain a copy of the
 *  License at http://gridengine.sunsource.net/Gridengine_SISSL_license.html
 * 
 *  Software provided under this License is provided on an "AS IS" basis,
 *  WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING,
 *  WITHOUT LIMITATION, WARRANTIES THAT THE SOFTWARE IS FREE OF DEFECTS,
 *  MERCHANTABLE, FIT FOR A PARTICULAR PURPOSE, OR NON-INFRINGING.
 *  See the License for the specific provisions governing your rights and
 *  obligations concerning the Software.
 * 
 *   The Initial Developer of the Original Code is: Sun Microsystems, Inc.
 * 
 *   Copyright: 2001 by Sun Microsystems, Inc.
 * 
 *   All Rights Reserved.
 * 
 ************************************************************************/
/*___INFO__MARK_END__*/
#include <sys/types.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifndef WIN32
#   include <malloc.h>
#endif

#include "sgermon.h"
#include "sge.h"
#include "sge_gdi_intern.h"
#include "cull.h"
#include "sge_all_listsL.h"
#include "sge_parse_num_par.h"
#include "sort_hosts.h"
#include "sge_complex_schedd.h"
#include "sge_sched.h"
#include "sge_feature.h"
#include "sge_string.h"
#include "sge_log.h"
#include "msg_schedd.h"
static char load_ops[]={
        '+',
        '-',
        '*',
        '/',
        '&',
        '|',
        '^',
        '\0'
};


enum {
        LOAD_OP_NONE=-1,
        LOAD_OP_PLUS,
        LOAD_OP_MINUS,
        LOAD_OP_TIMES,
        LOAD_OP_DIV,
        LOAD_OP_AND,
        LOAD_OP_OR,
        LOAD_OP_XOR
};

/* prototypes */
static double scaled_mixed_load(lList *tcl);
static int get_load_value(double *dval, lList *tcl, char *name);

/*************************************************************************

   sort_host_list

   purpose:
      sort host list according to a load evaluation formula.

   return values:
      0 on success; -1 otherwise

   input parameters:
      hl             :  the host list to be sorted
      cplx_list      :  the complex list
      formula        :  the load evaluation formula (containing no blanks)

   output parameters:
      hl             :  the sorted host list

*************************************************************************/

int sort_host_list(
lList *hl,           /* EH_Type */ 
lList *cplx_list     /* CX_Type */
) {
   lListElem *hlp;
   const char *host;
   double load;
   lListElem *host_complex;
   lList *host_complex_attributes = NULL, *tcl = NULL;
   
   DENTER(TOP_LAYER, "sort_host_list");

   /* 
      don't panic if there is no host_complex 
      or a given attributename does not exist -
      error handling is done in get_load_value()
   */
   if ((host_complex = lGetElemStr(cplx_list, CX_name, SGE_HOST_NAME)))
      host_complex_attributes = lGetList(host_complex, CX_entries);

   for_each (hlp, hl) {
      host = lGetHost(hlp,EH_name);
      if (strcmp(host,"global")) { /* don't treat global */

         /* build complexes for that host */
         host_complexes2scheduler(&tcl, hlp, hl, cplx_list, 0);
         lSetDouble(hlp, EH_sort_value, load = scaled_mixed_load(tcl));
         tcl = lFreeList(tcl);  
         DPRINTF(("%s: %f\n", lGetHost(hlp, EH_name), load));
      }
   }

   if (lPSortList(hl,"%I+",EH_sort_value)) {
      DEXIT;
      return -1;
   } else {
      DEXIT;
      return 0;
   }
}


/*************************************************************************
   scaled_mixed_load:

   purpose:
      compute scaled and weighted load for a particular host according
      to a load formula and that host's load and load scaling lists.

   return value:
      load value to be used for sorting the host list. in case of
      errors, a value of ERROR_LOAD_VAL is returned thereby ensuring
      that hosts with incorrect load reporting are considered heavily
      loaded.

   input paramters:
      load_list      : the host's load list  -> to be passed further
      scaling_list   : the host's load scaling list   -> to be passed
                       further
      host_cplx      : the entries list of the host complex -> to be
                       passed further
      lc_factor      : factor that is used to implement load correction: 
                       0 means no load correction, 
                       n load correction for n new jobs
*************************************************************************/
static double scaled_mixed_load(
lList *tcl 
) {
   char *cp, *tf, *ptr, *ptr2, *par_name, *op_ptr=NULL;
   double val=0, val2=0;
   double load=0;
   int op_pos, next_op=LOAD_OP_NONE;

   DENTER(TOP_LAYER, "scaled_mixed_load");

   /* we'll use strtok ==> we need a safety copy */
   if (!(tf = strdup(scheddconf.load_formula))) {
      DEXIT;
      return ERROR_LOAD_VAL;
   }

   /* + and - have the lowest precedence. all else are equal,
    * thus formula is delimited by + or - signs
    */
   for (cp=strtok(tf, "+-"); cp; cp = strtok(NULL, "+-")) {

      /* ---------------------------------------- */
      /* get scaled load value                    */
      /* determine 1st components value           */
      if (!(val = strtol(cp, &ptr, 0)) && ptr == cp) {
         /* it is not an integer ==> it's got to be a load value */
         if (!(par_name = sge_delim_str(cp,&ptr,load_ops)) ||
               get_load_value(&val, tcl, par_name)) {
            if (par_name)
               free(par_name);
            free(tf);
            DEXIT;
            return ERROR_LOAD_VAL;
         }
         free(par_name);
         par_name=NULL;
      }

      /* ---------------------------------------- */
      /* for the load value                       */
      /* *ptr now contains the delimiting character for val */
      if (*ptr) {
         /* if the delimiter is not \0 it's got to be a operator -> find it */
         if (!(op_ptr=strchr(load_ops,(int) *ptr))) {
            free(tf);
            DEXIT;
            return ERROR_LOAD_VAL;
         }
         op_pos = (int) (op_ptr - load_ops);

         /* ------------------------------- */
         /* look for a weightening factors  */
         /* determine 2nd component's value */
         ptr++;
         if (!(val2 = (double)strtol(ptr,&ptr2,0)) && ptr2 == ptr) {
            /* it is not an integer ==> it's got to be a load value */
            if (!(par_name = sge_delim_str(ptr,NULL,load_ops)) ||
               get_load_value(&val2, tcl, par_name)) {
               if (par_name)
                  free(par_name);
               free(tf);
               DEXIT;
               return ERROR_LOAD_VAL;
            }
            free(par_name);
            par_name=NULL;
         }

         /* ------------------------------- */
         /*  apply according load operator  */
         switch (op_pos) {
            case LOAD_OP_TIMES:
               val *= val2;
               break;
            case LOAD_OP_DIV:
               val /= val2;
               break;
            case LOAD_OP_AND: {
               u_long32 tmp;
               tmp = (u_long32)val & (u_long32)val2;
               val = (double)tmp;
               break;
            }
            case LOAD_OP_OR: {
               u_long32 tmp;
               tmp = (u_long32)val | (u_long32)val2; 
               val = (double)tmp;
               break;
            }
            case LOAD_OP_XOR: {
               u_long32 tmp;
               tmp = (u_long32)val ^ (u_long32)val2;
               val = (double)tmp;
               break;
            }
         }     /* switch (op_pos) */
      }     /* if (*ptr) */
   
      /* now we have the intermediate result from the subexpression in
       * between a + or - operator in val. next we've to add or
       * subtract from the current result value.
       */

      /* next_op is the next operation from the last run of the while loop.
       * thus next_op now is the operation to applied
       */
      switch (next_op) {
         case LOAD_OP_NONE:
            /* this is the first run -> just set load */
            load = val;
            break;
         case LOAD_OP_PLUS:
            load += val;
            break;
         case LOAD_OP_MINUS:
            load -= val;
            break;
      }

      /* determine next_op from the safety copy of the stripped formula */
      if (scheddconf.load_formula[cp-tf+strlen(cp)] == '+')
         next_op = LOAD_OP_PLUS;
      else
         next_op = LOAD_OP_MINUS;
   }

   free(tf);
   DEXIT;
   return load;
}



/***********************************************************************

   get_load_value

 ***********************************************************************/
static int get_load_value(
double *dvalp,
lList *tcl,
char *name 
) {
   lListElem *cep;

   DENTER(TOP_LAYER, "get_load_value");

   /* search complex */
   if (!(cep = lGetElemStr(tcl, CE_name, name))) {
      /* admin has forgotten to configure complex for load value in load formula */
      ERROR((SGE_EVENT, MSG_ATTRIB_NOATTRIBXINCOMPLEXLIST_S , name));
      DEXIT;
      return 1;
   }

   if ((lGetUlong(cep, CE_pj_dominant)&DOMINANT_TYPE_VALUE)) 
      *dvalp = lGetDouble(cep, CE_doubleval);
   else
      *dvalp = lGetDouble(cep, CE_pj_doubleval);

   DEXIT;
   return 0;
}


int debit_job_from_hosts(
lListElem *job,     /* JB_Type */
lList *granted,     /* JG_Type */
lList *host_list,   /* EH_Type */
lList *complex_list, /* CX_Type */
int *sort_hostlist
) {
   lSortOrder *so = NULL;
   lListElem *gel, *hep;
   const char *hnm;
   double old_sort_value, new_sort_value;

   DENTER(TOP_LAYER, "debit_job_from_hosts");

   if (feature_is_enabled(FEATURE_SGEEE) 
       || scheddconf.queue_sort_method!=QSM_SHARE) {
      so = lParseSortOrderVarArg(lGetListDescr(host_list), "%I+", EH_sort_value);
   }

   /* debit from hosts */
   for_each(gel, granted) {
      u_long32 ulc_factor;
      lList *tcl;
      int slots = lGetUlong(gel, JG_slots);

      hnm = lGetHost(gel, JG_qhostname);
      hep = lGetElemHost(host_list, EH_name, hnm); 

      if (scheddconf.load_adjustment_decay_time && lGetNumberOfElem(scheddconf.job_load_adjustments)) {
         /* increase host load for each scheduled job slot */
         ulc_factor = lGetUlong(hep, EH_load_correction_factor);
         ulc_factor += 100*slots;
         lSetUlong(hep, EH_load_correction_factor, ulc_factor);
      }   

      debit_host_consumable(job, lGetElemHost(host_list, EH_name, "global"), complex_list, slots);
      debit_host_consumable(job, hep, complex_list, slots);

      /* compute new combined load for this host and put it into the host */
      old_sort_value = lGetDouble(hep, EH_sort_value); 
      tcl = NULL;
      host_complexes2scheduler(&tcl, hep, host_list, complex_list, 0);
      new_sort_value = scaled_mixed_load(tcl);
      if(new_sort_value != old_sort_value) {
         lSetDouble(hep, EH_sort_value, new_sort_value);
         *sort_hostlist = 1;
         DPRINTF(("Increasing sort value of Host %s from %f to %f\n", 
            hnm, old_sort_value, new_sort_value));
      }
      tcl = lFreeList(tcl);  

      if (feature_is_enabled(FEATURE_SGEEE) 
          || scheddconf.queue_sort_method!=QSM_SHARE) {
         /* change position of this host in the host_list */
         /* !!! JG: sorting always necessary, or only if sort_hostlist? */
         lResortElem(so, hep, host_list);
      }
   }

   if(so) {
      lFreeSortOrder(so);
   }   

   DEXIT; 
   return 0;
}

int debit_host_consumable(
lListElem *jep,      /* JB_Type */
lListElem *hep,      /* EH_Type */
lList *complex_list, /* CX_Type */
int slots 
) {
   return debit_consumable(jep, hep, complex_list, slots,
         EH_consumable_config_list, EH_consumable_actual_list, 
         lGetHost(hep, EH_name));
}
