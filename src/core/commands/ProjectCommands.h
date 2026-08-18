#pragma once

#include "core/Project.h"

#include <QUndoCommand>

namespace TonDron {

// Restores a full project snapshot on undo/redo.
class ProjectSnapshotCommand : public QUndoCommand
{
public:
    ProjectSnapshotCommand(Project *project, Project before, Project after, const QString &text);

    void undo() override;
    void redo() override;

private:
    Project *m_project = nullptr;
    Project m_before;
    Project m_after;
};

} // namespace TonDron
