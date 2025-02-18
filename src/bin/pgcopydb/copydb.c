/*
 * src/bin/pgcopydb/copydb.c
 *     Implementation of a CLI to copy a database between two Postgres instances
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <unistd.h>

#include "parson.h"

#include "cli_common.h"
#include "copydb.h"
#include "env_utils.h"
#include "lock_utils.h"
#include "log.h"
#include "parsing_utils.h"
#include "pidfile.h"
#include "schema.h"
#include "signals.h"
#include "string_utils.h"
#include "summary.h"


/* Postgres 9.5 does not have idle_in_transaction_session_timeout */
GUC srcSettings95[] = {
	COMMON_GUC_SETTINGS,
	{ NULL, NULL },
};


GUC srcSettings[] = {
	COMMON_GUC_SETTINGS,
	{ "idle_in_transaction_session_timeout", "0" },
	{ NULL, NULL },
};


GUC dstSettings[] = {
	COMMON_GUC_SETTINGS,
	{ "maintenance_work_mem", "'1 GB'" },
	{ "synchronous_commit", "'off'" },
	{ "statement_timeout", "0" },
	{ "lock_timeout", "0" },
	{ NULL, NULL },
};


/*
 * Not used at the moment. We would like to ensure those settings have values
 * well-suited for the bulk loading operation, but we can't change those
 * settings on the fly.
 */
GUC serverSetttings[] = {
	{ "checkpoint_timeout", "'1 h'" },
	{ "max_wal_size", "' 20 GB'" },
	{ NULL, NULL },
};


/*
 * These parameters are added to the connection strings, unless the user has
 * added them, allowing user-defined values to be taken into account.
 */
KeyVal connStringDefaults = {
	.count = 4,
	.keywords = {
		"keepalives",
		"keepalives_idle",
		"keepalives_interval",
		"keepalives_count"
	},
	.values = {
		"1",
		"10",
		"10",
		"60"
	}
};


/*
 * copydb_init_tempdir initialises the file paths that are going to be used to
 * store temporary information while the pgcopydb process is running.
 */
bool
copydb_init_workdir(CopyDataSpec *copySpecs,
					char *dir,
					bool service,
					char *serviceName,
					bool restart,
					bool resume,
					bool createWorkDir)
{
	CopyFilePaths *cfPaths = &(copySpecs->cfPaths);
	DirectoryState *dirState = &(copySpecs->dirState);

	if (!copydb_prepare_filepaths(cfPaths, dir, serviceName))
	{
		/* errors have already been logged */
		return false;
	}

	log_notice("Using work dir \"%s\"", cfPaths->topdir);

	/*
	 * Some inspection commands piggy-back on the work directory that has been
	 * created by the main pgcopydb command, so it expects the work directory
	 * to have been created already.
	 */
	if (!createWorkDir && !directory_exists(cfPaths->topdir))
	{
		log_fatal("Work directory \"%s\" does not exists", cfPaths->topdir);
		return false;
	}

	bool removeDir = false;

	if (restart)
	{
		removeDir = true;
	}
	else
	{
		if (!copydb_inspect_workdir(cfPaths, dirState))
		{
			/* errors have already been logged */
			return false;
		}

		if (dirState->directoryExists)
		{
			/* if we did nothing yet, just act as if --resume was used */
			if (!dirState->schemaDumpIsDone)
			{
				log_notice("Schema dump has not been done yet, just continue");
			}

			/* if --resume has been used, we just continue */
			else if (resume)
			{
				/* no-op */
				(void) 0;
			}
			else if (dirState->allDone)
			{
				log_fatal("Please use --restart to allow for removing files "
						  "that belong to a completed previous run.");
				return false;
			}
			else if (!resume)
			{
				log_fatal("Please use --resume --not-consistent to allow "
						  "for resuming from the previous run, "
						  "which failed before completion.");
				return false;
			}

			/*
			 * Here we should have restart true or resume true or we didn't even do
			 * the schema dump on the previous run.
			 */
		}
	}

	/* warn about trashing data from a previous run */
	if (removeDir && !restart)
	{
		log_notice("Inspection of \"%s\" shows that it is safe "
				   "to remove it and continue",
				   cfPaths->topdir);
	}

	if (removeDir)
	{
		log_notice("Removing directory \"%s\"", cfPaths->topdir);
	}

	/* make sure the directory exists, possibly making it empty */
	if (!copydb_rmdir_or_mkdir(cfPaths->topdir, removeDir))
	{
		/* errors have already been logged */
		return false;
	}

	/* protect against running multiple "service" commands concurrently */
	if (service)
	{
		if (!copydb_acquire_pidfile(cfPaths, serviceName))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/* and now for the other sub-directories */
	const char *dirs[] = {
		cfPaths->schemadir,
		cfPaths->rundir,
		cfPaths->tbldir,
		cfPaths->idxdir,
		cfPaths->cdc.dir,
		cfPaths->compare.dir,
		NULL
	};

	for (int i = 0; dirs[i] != NULL; i++)
	{
		if (!copydb_rmdir_or_mkdir(dirs[i], removeDir))
		{
			/* errors have already been logged */
			return false;
		}
	}

	return true;
}


/*
 * copydb_acquire_pidfile deals with creating the pidfile for the current
 * service, which is the "main" pgcopydb service unless serviceName is not
 * NULL.
 */
bool
copydb_acquire_pidfile(CopyFilePaths *cfPaths, char *serviceName)
{
	if (!directory_exists(cfPaths->topdir))
	{
		log_fatal("Work directory \"%s\" does not exists", cfPaths->topdir);
		return false;
	}

	pid_t pid = getpid();

	/*
	 * Only create the main pidfile when we're not running an auxilliary
	 * service.
	 */
	if (serviceName == NULL)
	{
		if (!copydb_create_pidfile(cfPaths->pidfile, pid, true))
		{
			/* errors have already been logged */
			return false;
		}

		return true;
	}

	/*
	 * The "snapshot" service is special, it's an auxilliary service that's
	 * allowed to run concurrently to the "main" pgcopydb service.
	 */
	if (!streq("snapshot", serviceName))
	{
		if (!copydb_create_pidfile(cfPaths->pidfile, pid, false))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/*
	 * When running an auxilliary service, we create its own pidfile and also
	 * check that the same service isn't already running.
	 */
	if (!copydb_create_pidfile(cfPaths->spidfile, pid, true))
	{
		/* errors have already been logged */
		return false;
	}

	return true;
}


/*
 * copydb_create_pidfile creates a pidfile of the given name and updates the
 * given pid. It fails if the pidfile already exists and belongs to a currently
 * running process, acting like a lockfile then.
 */
bool
copydb_create_pidfile(const char *pidfile, pid_t pid, bool createPidFile)
{
	if (file_exists(pidfile))
	{
		pid_t onFilePid = 0;

		if (read_pidfile(pidfile, &onFilePid))
		{
			log_fatal("Pidfile \"%s\" already exists with process %d, "
					  "which is currently running",
					  pidfile,
					  onFilePid);
			return false;
		}
	}

	/* now populate our pidfile */
	if (createPidFile)
	{
		if (!create_pidfile(pidfile, pid))
		{
			return false;
		}
	}

	return true;
}


/*
 * copydb_inspect_workdir inspects the given target directory to see what work
 * has been tracked in there. From the doneFile(s) and the lockFile(s) that we
 * can list in the directory, we can have a good idea of why the command is
 * attempted to be run again.
 */
bool
copydb_inspect_workdir(CopyFilePaths *cfPaths, DirectoryState *dirState)
{
	dirState->directoryExists = directory_exists(cfPaths->topdir);

	if (!dirState->directoryExists)
	{
		return true;
	}

	/* the directory exists, checks if our expected components are there */
	bool foundAllComponents = true;

	const char *dirs[] = {
		cfPaths->schemadir,
		cfPaths->rundir,
		cfPaths->tbldir,
		cfPaths->idxdir,
		NULL
	};

	for (int i = 0; dirs[i] != NULL; i++)
	{
		foundAllComponents = foundAllComponents && directory_exists(dirs[i]);
	}

	if (!foundAllComponents)
	{
		log_debug("copydb_inspect_workdir: not all components found");
		dirState->directoryIsReady = false;
		return true;
	}

	dirState->schemaDumpIsDone =
		file_exists(cfPaths->done.preDataDump) &&
		file_exists(cfPaths->done.postDataDump);

	dirState->schemaPreDataHasBeenRestored =
		file_exists(cfPaths->done.preDataRestore);

	dirState->schemaPostDataHasBeenRestored =
		file_exists(cfPaths->done.postDataRestore);

	dirState->tableCopyIsDone = file_exists(cfPaths->done.tables);
	dirState->indexCopyIsDone = file_exists(cfPaths->done.indexes);
	dirState->sequenceCopyIsDone = file_exists(cfPaths->done.sequences);
	dirState->blobsCopyIsDone = file_exists(cfPaths->done.blobs);

	dirState->allDone =
		dirState->schemaDumpIsDone &&
		dirState->schemaPreDataHasBeenRestored &&
		dirState->schemaPostDataHasBeenRestored &&
		dirState->tableCopyIsDone &&
		dirState->indexCopyIsDone &&
		dirState->sequenceCopyIsDone &&
		dirState->blobsCopyIsDone;

	/* let's be verbose about our inspection results */
	log_notice("Work directory \"%s\" already exists", cfPaths->topdir);

	if (dirState->allDone)
	{
		log_info("A previous run has run through completion");
		return true;
	}

	if (dirState->schemaDumpIsDone)
	{
		log_info("Schema dump for pre-data and post-data section have been done");
	}

	if (dirState->schemaPreDataHasBeenRestored)
	{
		log_info("Pre-data schema has been restored on the target instance");
	}

	if (dirState->tableCopyIsDone)
	{
		log_info("All the table data has been copied to the target instance");
	}

	if (dirState->indexCopyIsDone)
	{
		log_info("All the indexes have been copied to the target instance");
	}

	if (dirState->sequenceCopyIsDone)
	{
		log_info("All the sequences have been copied to the target instance");
	}

	if (dirState->blobsCopyIsDone)
	{
		log_info("All the large objects have been copied to the target instance");
	}

	if (dirState->schemaPostDataHasBeenRestored)
	{
		log_info("Post-data schema has been restored on the target instance");
	}

	return true;
}


/*
 * copydb_prepare_filepaths computes all the path components that are needed
 * for top-level operations.
 */
bool
copydb_prepare_filepaths(CopyFilePaths *cfPaths,
						 const char *dir,
						 const char *serviceName)
{
	char topdir[MAXPGPATH] = { 0 };

	if (dir != NULL && !IS_EMPTY_STRING_BUFFER(dir))
	{
		strlcpy(topdir, dir, sizeof(topdir));
	}
	else
	{
		char tmpdir[MAXPGPATH] = { 0 };

		if (!get_env_copy_with_fallback("TMPDIR",
										tmpdir,
										sizeof(tmpdir),
										"/tmp"))
		{
			/* errors have already been logged */
			return false;
		}

		sformat(topdir, sizeof(topdir), "%s/pgcopydb", tmpdir);
	}

	/* first copy the top directory */
	strlcpy(cfPaths->topdir, topdir, sizeof(cfPaths->topdir));

	/* some processes use an additional per-service pidfile */
	if (serviceName != NULL)
	{
		sformat(cfPaths->spidfile, MAXPGPATH, "%s/pgcopydb.%s.pid",
				cfPaths->topdir,
				serviceName);
	}
	sformat(cfPaths->pidfile, MAXPGPATH, "%s/pgcopydb.pid", cfPaths->topdir);

	/* now that we have our topdir, prepare all the others from there */
	sformat(cfPaths->snfile, MAXPGPATH, "%s/snapshot", cfPaths->topdir);
	sformat(cfPaths->schemadir, MAXPGPATH, "%s/schema", cfPaths->topdir);
	sformat(cfPaths->rundir, MAXPGPATH, "%s/run", cfPaths->topdir);
	sformat(cfPaths->tbldir, MAXPGPATH, "%s/run/tables", cfPaths->topdir);
	sformat(cfPaths->idxdir, MAXPGPATH, "%s/run/indexes", cfPaths->topdir);

	/* prepare also the name of the schema file (JSON) */
	sformat(cfPaths->schemafile, MAXPGPATH, "%s/schema.json", cfPaths->topdir);

	/* prepare also the name of the summary file (JSON) */
	sformat(cfPaths->summaryfile, MAXPGPATH, "%s/summary.json", cfPaths->topdir);

	/* now prepare the done files */
	struct pair
	{
		char *dst;
		char *fmt;
	};

	struct pair donePaths[] = {
		{ (char *) &(cfPaths->done.preDataDump), "%s/run/dump-pre.done" },
		{ (char *) &(cfPaths->done.postDataDump), "%s/run/dump-post.done" },
		{ (char *) &(cfPaths->done.preDataRestore), "%s/run/restore-pre.done" },
		{ (char *) &(cfPaths->done.postDataRestore), "%s/run/restore-post.done" },

		{ (char *) &(cfPaths->done.tables), "%s/run/tables.done" },
		{ (char *) &(cfPaths->done.indexes), "%s/run/indexes.done" },
		{ (char *) &(cfPaths->done.sequences), "%s/run/sequences.done" },
		{ (char *) &(cfPaths->done.blobs), "%s/run/blobs.done" },
		{ NULL, NULL }
	};

	for (int i = 0; donePaths[i].dst != NULL; i++)
	{
		sformat(donePaths[i].dst, MAXPGPATH, donePaths[i].fmt, cfPaths->topdir);
	}

	/*
	 * Now prepare the Change Data Capture (logical decoding) intermediate
	 * files directory. This needs more care than the transient files that
	 * default to the TMPDIR (or /tmp), and we're using XDG_DATA_HOME this time
	 * (/var, or ~/.local/share).
	 *
	 * When a directory has been provided, use a sub-directory there to store
	 * the Change Data Capture date. Otherwise, use a pgcopydb specific
	 * directory in ~/.local/share or XDG_DATA_HOME.
	 */
	if (dir != NULL && !IS_EMPTY_STRING_BUFFER(dir))
	{
		sformat(cfPaths->cdc.dir, MAXPGPATH, "%s/cdc", cfPaths->topdir);
	}
	else
	{
		char homedir[MAXPGPATH] = { 0 };
		char datadir[MAXPGPATH] = { 0 };
		char fallback[MAXPGPATH] = { 0 };

		if (!get_env_copy("HOME", homedir, MAXPGPATH))
		{
			/* errors have already been logged */
			return false;
		}

		join_path_components(fallback, homedir, ".local/share");

		if (!get_env_copy_with_fallback("XDG_DATA_HOME",
										datadir,
										sizeof(datadir),
										fallback))
		{
			/* errors have already been logged */
			return false;
		}

		sformat(cfPaths->cdc.dir, MAXPGPATH, "%s/pgcopydb", datadir);
	}

	log_debug("Change Data Capture data is managed at \"%s\"",
			  cfPaths->cdc.dir);

	/* now prepare the originfile and timelinehistfile path */
	sformat(cfPaths->cdc.originfile, MAXPGPATH,
			"%s/origin",
			cfPaths->cdc.dir);

	sformat(cfPaths->cdc.slotfile, MAXPGPATH,
			"%s/slot",
			cfPaths->cdc.dir);

	sformat(cfPaths->cdc.tlihistfile, MAXPGPATH,
			"%s/tli.history",
			cfPaths->cdc.dir);

	sformat(cfPaths->cdc.tlifile, MAXPGPATH,
			"%s/tli",
			cfPaths->cdc.dir);

	sformat(cfPaths->cdc.walsegsizefile, MAXPGPATH,
			"%s/wal_segment_size",
			cfPaths->cdc.dir);

	sformat(cfPaths->cdc.lsntrackingfile, MAXPGPATH,
			"%s/lsn.json",
			cfPaths->cdc.dir);

	/*
	 * Now prepare the "compare" files we need to compare schema and data
	 * between the source and target instance.
	 */
	sformat(cfPaths->compare.dir, MAXPGPATH, "%s/compare", cfPaths->topdir);

	sformat(cfPaths->compare.sschemafile, MAXPGPATH,
			"%s/source-schema.json",
			cfPaths->compare.dir);

	sformat(cfPaths->compare.tschemafile, MAXPGPATH,
			"%s/target-schema.json",
			cfPaths->compare.dir);

	sformat(cfPaths->compare.sdatafile, MAXPGPATH,
			"%s/source-data.json",
			cfPaths->compare.dir);

	sformat(cfPaths->compare.tdatafile, MAXPGPATH,
			"%s/target-data.json",
			cfPaths->compare.dir);

	return true;
}


/*
 * copydb_prepare_dump_paths computes the paths for the pg_dump and pg_restore
 * activities.
 */
bool
copydb_prepare_dump_paths(CopyFilePaths *cfPaths, DumpPaths *dumpPaths)
{
	sformat(dumpPaths->rolesFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "roles.sql");

	sformat(dumpPaths->extnspFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "extnamespaces.dump");

	sformat(dumpPaths->preFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "pre.dump");

	sformat(dumpPaths->preListOutFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "pre-out.list");

	sformat(dumpPaths->preListFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "pre-filtered.list");

	sformat(dumpPaths->postFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "post.dump");

	sformat(dumpPaths->postListOutFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "post-out.list");

	sformat(dumpPaths->postListFilename, MAXPGPATH, "%s/%s",
			cfPaths->schemadir, "post-filtered.list");

	return true;
}


/*
 * copydb_rmdir_or_mkdir ensure that given directory is empty. For that it
 * either uses rm -rf on an existing directory or just mkdir -p on a possibly
 * existing directory, depending on the removeDir argument.
 */
bool
copydb_rmdir_or_mkdir(const char *dir, bool removeDir)
{
	if (removeDir)
	{
		log_debug("rm -rf \"%s\" && mkdir -p \"%s\"", dir, dir);

		if (!ensure_empty_dir(dir, 0700))
		{
			return false;
		}
	}
	else
	{
		if (!directory_exists(dir))
		{
			log_debug("mkdir -p \"%s\"", dir);
		}

		if (pg_mkdir_p((char *) dir, 0700) == -1)
		{
			log_fatal("Failed to create directory \"%s\": %m", dir);
			return false;
		}
	}

	return true;
}


/*
 * copydb_init_specs prepares a CopyDataSpec structure from its pieces, and
 * initialises files paths necessary for collecting a Postgres dump splitted in
 * pre-data and post-data section, and then also a pg_restore --list output
 * file.
 */
bool
copydb_init_specs(CopyDataSpec *specs,
				  CopyDBOptions *options,
				  CopyDataSection section)
{
	/* fill-in a structure with the help of the C compiler */
	CopyDataSpec tmpCopySpecs = {
		.cfPaths = specs->cfPaths,
		.pgPaths = specs->pgPaths,

		.connStrings = options->connStrings,

		.sourceSnapshot = {
			.pgsql = { 0 },
			.pguri = options->connStrings.source_pguri,
			.safeURI = options->connStrings.safeSourcePGURI,
			.connectionType = PGSQL_CONN_SOURCE,
			.snapshot = { 0 }
		},

		.section = section,
		.restoreOptions = options->restoreOptions,
		.roles = options->roles,
		.skipLargeObjects = options->skipLargeObjects,
		.skipExtensions = options->skipExtensions,
		.skipCommentOnExtension = options->skipCommentOnExtension,
		.skipCollations = options->skipCollations,
		.skipVacuum = options->skipVacuum,
		.noRolesPasswords = options->noRolesPasswords,
		.failFast = options->failFast,

		.restart = options->restart,
		.resume = options->resume,
		.consistent = !options->notConsistent,

		.tableJobs = options->tableJobs,
		.indexJobs = options->indexJobs,
		.lObjectJobs = options->lObjectJobs,

		/* at the moment we don't have --vacuumJobs separately */
		.vacuumJobs = options->tableJobs,

		.splitTablesLargerThan = options->splitTablesLargerThan,

		.tableSemaphore = { 0 },
		.indexSemaphore = { 0 },

		.vacuumQueue = { 0 },
		.indexQueue = { 0 },

		.catalog = { 0 },
		.tableSpecsArray = { 0, NULL }
	};

	if (!IS_EMPTY_STRING_BUFFER(options->snapshot))
	{
		strlcpy(tmpCopySpecs.sourceSnapshot.snapshot,
				options->snapshot,
				sizeof(tmpCopySpecs.sourceSnapshot.snapshot));
	}

	/* copy the structure as a whole memory area to the target place */
	*specs = tmpCopySpecs;

	/* now compute some global paths that are needed for pgcopydb */
	if (!copydb_prepare_dump_paths(&(specs->cfPaths), &(specs->dumpPaths)))
	{
		/* errors have already been logged */
		return false;
	}

	/* create the table semaphore (critical section, one at a time please) */
	specs->tableSemaphore.initValue = 1;

	if (!semaphore_create(&(specs->tableSemaphore)))
	{
		log_error("Failed to create the table concurrency semaphore "
				  "to orchestrate %d TABLE DATA COPY jobs",
				  options->tableJobs);
		return false;
	}

	/* create the index semaphore (critical section, one at a time please) */
	specs->indexSemaphore.initValue = 1;

	if (!semaphore_create(&(specs->indexSemaphore)))
	{
		log_error("Failed to create the index concurrency semaphore "
				  "to orchestrate %d CREATE INDEX jobs",
				  options->indexJobs);
		return false;
	}

	if (specs->section == DATA_SECTION_ALL ||
		specs->section == DATA_SECTION_TABLE_DATA)
	{
		/* create the VACUUM process queue */
		if (!specs->skipVacuum)
		{
			if (!queue_create(&(specs->vacuumQueue), "vacuum"))
			{
				log_error("Failed to create the VACUUM process queue");
				return false;
			}
		}

		/* create the CREATE INDEX process queue */
		if (!queue_create(&(specs->indexQueue), "create index"))
		{
			log_error("Failed to create the INDEX process queue");
			return false;
		}
	}

	/* we only respect the --skip-blobs option in pgcopydb copy-db command */
	if (specs->section != DATA_SECTION_ALL)
	{
		specs->skipLargeObjects = true;
	}

	return true;
}


/*
 * copydb_init_table_specs prepares a CopyTableDataSpec structure from its
 * pieces and also initialises files paths necessary for the orchestration of
 * the per-table processes and their summary files.
 */
bool
copydb_init_table_specs(CopyTableDataSpec *tableSpecs,
						CopyDataSpec *specs,
						SourceTable *source,
						int partNumber)
{
	/* fill-in a structure with the help of the C compiler */
	CopyTableDataSpec tmpTableSpecs = {
		.cfPaths = &(specs->cfPaths),
		.pgPaths = &(specs->pgPaths),

		.connStrings = &(specs->connStrings),

		.section = specs->section,
		.resume = specs->resume,

		.sourceTable = source,
		.indexArray = NULL,
		.summary = NULL,

		.tableJobs = specs->tableJobs,
		.indexJobs = specs->indexJobs,

		.indexSemaphore = &(specs->indexSemaphore)
	};

	/* copy the structure as a whole memory area to the target place */
	*tableSpecs = tmpTableSpecs;

	/* This CopyTableDataSpec might be for a partial COPY */
	if (source->partsArray.count >= 1)
	{
		CopyTableDataPartSpec part = {
			.partNumber = partNumber,
			.partCount = source->partsArray.array[partNumber].partCount,
			.min = source->partsArray.array[partNumber].min,
			.max = source->partsArray.array[partNumber].max
		};

		tableSpecs->part = part;

		strlcpy(tableSpecs->part.partKey, source->partKey, NAMEDATALEN);

		/* now compute the table-specific paths we are using in copydb */
		if (!copydb_init_tablepaths_for_part(tableSpecs->cfPaths,
											 &(tableSpecs->tablePaths),
											 tableSpecs->sourceTable->oid,
											 partNumber))
		{
			log_error("Failed to prepare pathnames for partition %d of table %s",
					  partNumber,
					  tableSpecs->sourceTable->qname);
			return false;
		}

		/* used only by one process, the one finishing a partial COPY last */
		sformat(tableSpecs->tablePaths.idxListFile, MAXPGPATH, "%s/%u.idx",
				tableSpecs->cfPaths->tbldir,
				source->oid);

		/*
		 * And now the truncateLockFile and truncateDoneFile, which are used to
		 * provide a critical section to the same-table concurrent processes.
		 */
		sformat(tableSpecs->tablePaths.truncateDoneFile, MAXPGPATH,
				"%s/%u.truncate",
				tableSpecs->cfPaths->tbldir,
				source->oid);
	}
	else
	{
		/* No partition found, so this should be a full table COPY */
		if (partNumber > 0)
		{
			log_error("BUG: copydb_init_table_specs partNumber is %d and "
					  "source table partArray.count is %d",
					  partNumber,
					  source->partsArray.count);
			return false;
		}

		/* now compute the table-specific paths we are using in copydb */
		if (!copydb_init_tablepaths(tableSpecs->cfPaths,
									&(tableSpecs->tablePaths),
									tableSpecs->sourceTable->oid))
		{
			log_error("Failed to prepare pathnames for table %u",
					  tableSpecs->sourceTable->oid);
			return false;
		}
	}

	return true;
}


/*
 * copydb_init_tablepaths computes the lockFile, doneFile, and idxListFile
 * pathnames for a given table oid and global cfPaths setup.
 */
bool
copydb_init_tablepaths(CopyFilePaths *cfPaths,
					   TableFilePaths *tablePaths,
					   uint32_t oid)
{
	sformat(tablePaths->lockFile, MAXPGPATH, "%s/%d",
			cfPaths->rundir,
			oid);

	sformat(tablePaths->doneFile, MAXPGPATH, "%s/%d.done",
			cfPaths->tbldir,
			oid);

	sformat(tablePaths->idxListFile, MAXPGPATH, "%s/%u.idx",
			cfPaths->tbldir,
			oid);

	sformat(tablePaths->chksumFile, MAXPGPATH, "%s/%u.sum.json",
			cfPaths->tbldir,
			oid);

	return true;
}


/*
 * copydb_init_tablepaths_for_part computes the lockFile and doneFile pathnames
 * for a given COPY partition of a table.
 */
bool
copydb_init_tablepaths_for_part(CopyFilePaths *cfPaths,
								TableFilePaths *tablePaths,
								uint32_t oid,
								int partNumber)
{
	sformat(tablePaths->lockFile, MAXPGPATH, "%s/%d.%d",
			cfPaths->rundir,
			oid,
			partNumber);

	sformat(tablePaths->doneFile, MAXPGPATH, "%s/%d.%d.done",
			cfPaths->tbldir,
			oid,
			partNumber);

	return true;
}


/*
 * copydb_fatal_exit sends a termination signal to all the subprocess and waits
 * until all the known subprocess are finished, then returns true.
 */
bool
copydb_fatal_exit()
{
	log_fatal("Terminating all processes in our process group");

	/* signal all sub-processes that now is the time to stop */
	if (kill(0, SIGTERM) == -1)
	{
		log_error("Failed to signal pgcopydb process group: %m");
		return false;
	}

	/*
	 * Now wait until all the sub-processes have exited, and refrain from
	 * calling copydb_fatal_exit() recursively when a process exits with a
	 * non-zero return code.
	 */
	bool failFast = false;
	return copydb_wait_for_subprocesses(failFast);
}


/*
 * copydb_wait_for_subprocesses calls waitpid() until no child process is known
 * running. It also fetches the return code of all the sub-processes, and
 * returns true only when all the subprocesses have returned zero (success).
 */
bool
copydb_wait_for_subprocesses(bool failFast)
{
	bool allReturnCodeAreZero = true;
	log_debug("Waiting for sub-processes to finish");

	for (;;)
	{
		int status;

		/* ignore errors */
		pid_t pid = waitpid(-1, &status, WNOHANG);

		switch (pid)
		{
			case -1:
			{
				if (errno == ECHILD)
				{
					/* no more childrens */
					log_debug("copydb_wait_for_subprocesses: no more children");
					return allReturnCodeAreZero;
				}

				pg_usleep(100 * 1000); /* 100 ms */
				break;
			}

			case 0:
			{
				/*
				 * We're using WNOHANG, 0 means there are no stopped or exited
				 * children. Sleep for awhile and ask again later.
				 */
				pg_usleep(100 * 1000); /* 100 ms */
				break;
			}

			default:
			{
				int returnCode = WEXITSTATUS(status);

				if (returnCode == 0)
				{
					log_debug("Sub-process %d exited with code %d",
							  pid, returnCode);
				}
				else
				{
					allReturnCodeAreZero = false;

					log_error("Sub-process %d exited with code %d",
							  pid, returnCode);

					if (failFast)
					{
						log_error("Signaling other processes to terminate "
								  "(see --fail-fast)");
						(void) copydb_fatal_exit();
					}
				}

				break;
			}
		}
	}

	return allReturnCodeAreZero;
}


/*
 * copydb_register_sysv_semaphore registers a semaphore to our internal array
 * of System V resources for cleanup at exit.
 */
bool
copydb_register_sysv_semaphore(SysVResArray *array, Semaphore *semaphore)
{
	if (SYSV_RES_MAX_COUNT <= array->count)
	{
		log_fatal("Failed to register semaphore %d: "
				  "resource array counts %d items already",
				  semaphore->semId,
				  array->count);
		return false;
	}

	log_trace("copydb_register_sysv_semaphore[%d]: %d",
			  array->count,
			  semaphore->semId);

	array->array[array->count].kind = SYSV_SEMAPHORE;
	array->array[array->count].res.semaphore = semaphore;

	++(array->count);

	return true;
}


/*
 * copydb_unregister_sysv_queue marks the given queue as unlinked already.
 */
bool
copydb_unlink_sysv_semaphore(SysVResArray *array, Semaphore *semaphore)
{
	for (int i = 0; i < array->count; i++)
	{
		SysVRes *res = &(array->array[i]);

		if (res->kind == SYSV_SEMAPHORE &&
			res->res.semaphore->semId == semaphore->semId)
		{
			res->unlinked = true;
			return true;
		}
	}

	log_error("BUG: copydb_unlink_sysv_semaphore failed to find semaphore %d",
			  semaphore->semId);

	return false;
}


/*
 * copydb_register_sysv_queue registers a semaphore to our internal array of
 * System V resources for cleanup at exit.
 */
bool
copydb_register_sysv_queue(SysVResArray *array, Queue *queue)
{
	if (SYSV_RES_MAX_COUNT <= array->count)
	{
		log_fatal("Failed to register semaphore %d: "
				  "resource array counts %d items already",
				  queue->qId,
				  array->count);
		return false;
	}

	log_trace("copydb_register_sysv_queue[%d]: %d",
			  array->count,
			  queue->qId);

	array->array[array->count].kind = SYSV_QUEUE;
	array->array[array->count].res.queue = queue;

	++(array->count);

	return true;
}


/*
 * copydb_unregister_sysv_queue marks the given queue as unlinked already.
 */
bool
copydb_unlink_sysv_queue(SysVResArray *array, Queue *queue)
{
	for (int i = 0; i < array->count; i++)
	{
		SysVRes *res = &(array->array[i]);

		if (res->kind == SYSV_QUEUE && res->res.queue->qId == queue->qId)
		{
			res->unlinked = true;
			return true;
		}
	}

	log_error("BUG: copydb_unlink_sysv_queue failed to find queue %d",
			  queue->qId);

	return false;
}


/*
 * copydb_cleanup_sysv_resources unlinks semaphores and queues that have been
 * registered in the given array.
 */
bool
copydb_cleanup_sysv_resources(SysVResArray *array)
{
	pid_t pid = getpid();

	/*
	 * Clean-up resources in the reverse order of their registering.
	 *
	 * This is particulary important for the logging semaphore, which is the
	 * first resource that's registered in that array, and that we need until
	 * the very end.
	 */
	for (int i = array->count - 1; 0 <= i; i--)
	{
		SysVRes *res = &(array->array[i]);

		/* skip already unlinked System V resources */
		if (res->unlinked)
		{
			continue;
		}

		switch (res->kind)
		{
			case SYSV_QUEUE:
			{
				Queue *queue = res->res.queue;

				if (queue->owner == pid)
				{
					if (!queue_unlink(queue))
					{
						/* errors have already been logged */
						return false;
					}
				}

				break;
			}

			case SYSV_SEMAPHORE:
			{
				Semaphore *semaphore = res->res.semaphore;

				if (!semaphore_finish(semaphore))
				{
					/* errors have already been logged */
					return false;
				}

				break;
			}

			default:
			{
				log_error("BUG: Failed to clean-up System V resource "
						  " of unknown type: %d",
						  res->kind);
				return false;
			}
		}
	}

	return true;
}
