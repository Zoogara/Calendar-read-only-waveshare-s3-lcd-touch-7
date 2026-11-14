/* ui_detail.c compiled in here so the renderer can open the card for a named event without a touch
 * (its tagged-event table is private to that file). */
#include "../../components/calendar_ui/ui_detail.c"

bool host_detail_show(const char *summary_part)
{
    for (int i = 0; i < s_n; i++) {
        if (strstr(s_tab[i].summary, summary_part) != NULL) {
            return show_index(i);
        }
    }
    return false;
}
