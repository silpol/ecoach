/*
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA  02110-1301, USA.

    ---
    Copyright (C) 2008, Sampo Savola, Kai Skiftesvik
 */


#ifndef _CALCULATE_BMI_H
#define _CALCULATE_BMI_H
#include "navigation_menu.h"
#include <gtk/gtk.h>
#include "hildon/hildon-picker-button.h"

typedef struct _BMIData BMIData;

struct _BMIData{
    GtkWidget *val_label;
    gdouble bmi;
    gchar *char_bmi;
    GtkWidget *height_picker;
    GtkWidget *weight_picker;
    
    
    
    /*Weight picker*/
    
    	GtkWidget *weight_dialog;
	GtkWidget *weight_table;
	GtkWidget *weight_selector;
};


void show_calculate_bmi(NavigationMenu *menu, GtkTreePath *path,
				gpointer user_data);

#endif /* _CALCULATE_BMI_H */
