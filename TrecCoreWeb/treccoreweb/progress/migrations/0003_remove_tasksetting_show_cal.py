# -*- coding: utf-8 -*-
# Generated by Django 1.10.7 on 2017-07-02 16:46
from __future__ import unicode_literals

from django.db import migrations


class Migration(migrations.Migration):

    dependencies = [
        ('progress', '0002_auto_20170702_1629'),
    ]

    operations = [
        migrations.RemoveField(
            model_name='tasksetting',
            name='show_CAL',
        ),
    ]
